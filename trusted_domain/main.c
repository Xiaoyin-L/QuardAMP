#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include "debug_log.h"
#include "sbi.h"
#include "riscv_asm.h"
#include "ns16550.h"
#include "plic.h"
#include "quard_star.h"
#include "mailbox.h"
#include "icc.h"

extern QueueHandle_t xUartRxQueue;

#ifndef QUARDAMP_ACCEL_CLIENT
#define QUARDAMP_ACCEL_CLIENT 1
#endif

/*
 * UART RX 消费任务。
 * 阻塞在 xQueueReceive() 上等待 ISR 投递的字符。
 * 当 uart2 收到输入时：
 *   ISR handle_interrupt() -> xQueueSendFromISR()
 *   -> 唤醒本任务 -> portYIELD_FROM_ISR() 触发切换
 *   -> 本任务在任务上下文打印字符
 *
 * 优先级 5，高于普通 control-plane client 任务，保证 ISR 后尽快处理
 * RX 数据。
 */
static void vUartRxTask(void *p_arg)
{
    char ch;
    for(;;)
    {
        if (xQueueReceive(xUartRxQueue, &ch,
                          portMAX_DELAY) == pdTRUE)
        {
            debug_log("rx: %x '%c'\n", (unsigned long)(unsigned char)ch, ch);
        }
    }
}

#if QUARDAMP_ACCEL_CLIENT
#define ACCEL_BENCH_ITERS 32U
#define ACCEL_BENCH_LEN 128U
#define ACCEL_BENCH_GAP_MS 100U
#define ACCEL_BENCH_PERIOD_MS 10000U

static uint64_t accel_rdtime(void)
{
    uint64_t value;

    __asm__ volatile ("rdtime %0" : "=r" (value));
    return value;
}

static int accel_send_job(uint32_t job_id, uint32_t len, uint32_t opcode)
{
    struct rpmsg_hdr *msg;
    struct amp_accel_req req;

    req.type = SHMEM_CMD_ACCEL_SUBMIT;
    req.job_id = job_id;
    req.opcode = opcode;
    req.len = len;
    req.src_offset = 0;
    req.dst_offset = 0;
    req.client_submit_ticks = accel_rdtime();

    msg = icc_message_loan(SHMEM_EP_XV6_ACCEL);
    if (msg == NULL) {
        debug_log("accel client: loan failed job=%x\n",
                  (unsigned long)job_id);
        return -1;
    }

    icc_prepare_app_message(msg, SHMEM_EP_RTOS_ACCEL,
                            SHMEM_EP_XV6_ACCEL,
                            SHMEM_CMD_ACCEL_SUBMIT,
                            job_id, 0,
                            (const char *)&req,
                            sizeof(req));
    if (icc_message_send(msg) != 0) {
        debug_log("accel client: send failed job=%x\n",
                  (unsigned long)job_id);
        return -1;
    }

    debug_log("accel client: submit job=%x len=%d\n",
              (unsigned long)job_id, (int)req.len);
    return 0;
}

static void vAccelClientTask(void *p_arg)
{
    uint32_t job_id = 1;

    vTaskDelay(pdMS_TO_TICKS(7000));
    for (;;) {
        icc_accel_bench_reset();
        debug_log("accel bench: start iters=%d len=%d\n",
                  (int)ACCEL_BENCH_ITERS, (int)ACCEL_BENCH_LEN);

        for (uint32_t i = 0; i < ACCEL_BENCH_ITERS; i++) {
            if (accel_send_job(job_id, ACCEL_BENCH_LEN,
                               SHMEM_ACCEL_OP_XOR) == 0) {
                job_id++;
            }

            vTaskDelay(pdMS_TO_TICKS(ACCEL_BENCH_GAP_MS));
        }

        vTaskDelay(pdMS_TO_TICKS(500U));
        debug_log("accel bench: fault probe job=%x\n",
                  (unsigned long)job_id);
        if (accel_send_job(job_id, 0U, SHMEM_ACCEL_OP_XOR) == 0) {
            job_id++;
        }

        vTaskDelay(pdMS_TO_TICKS(ACCEL_BENCH_PERIOD_MS));
    }
}
#endif

static void vTaskCreate(void *p_arg)
{ 
	debug_log("vTaskCreate\n");

    xTaskCreate(vUartRxTask,"vUartRxTask",2048,NULL,5,NULL);
    xTaskCreate(vIccDispatchTask,"vIccDispatchTask",512,NULL,5,NULL);
    xTaskCreate(vIccNsTask,"vIccNsTask",512,NULL,4,NULL);
#if QUARDAMP_ACCEL_CLIENT
    xTaskCreate(vAccelClientTask,"vAccelClientTask",512,NULL,4,NULL);
#endif

    vTaskDelete(NULL);
}

int main(void)
{
    debug_log_init();
    debug_log("Hello FreeRTOS!\n");

    /*
     * PLIC 初始化要早于打开 S-mode external interrupt。
     * xPortStartScheduler() 中打开 sie.SEIE 后，PLIC pending 的外部中断
     * 就可能进入 handle_interrupt()。
     */
    plic_init_hart7();
    plic_enable_irq_hart7(UART2_IRQ);

    /*
     * 阶段 1：注册 mailbox 单向 doorbell（xv6 -> FreeRTOS）。
     * 设置 PLIC 源 13 优先级并在 hart7 context 使能。
     * 同样必须早于 xPortStartScheduler() 打开 sie.SEIE，
     * 否则 xv6 触发后中断无法投递到 handle_interrupt()。
     */
    mailbox_init();

    /*
     * 阶段 4：ICC 两级处理。
     * mailbox ISR 只负责把 to_rtos ring 搬入 xIccDispatchQueue，
     * 真正的 endpoint 分发和 echo 回复放在 vIccDispatchTask 中完成。
     */
    icc_init();
    if (icc_register_handler(SHMEM_EP_RTOS_ECHO, icc_echo_handler) != 0) {
        debug_log("icc: register echo handler failed\n");
    }
    /*
     * Register the regression RPC endpoint used by xv6 rpctest.
     * The handler runs in vIccDispatchTask context, not in the mailbox ISR,
     * so doing byte-wise payload conversion and sending a reply is safe here.
     */
    if (icc_register_handler(SHMEM_EP_RTOS_UPPER, icc_upper_handler) != 0) {
        debug_log("icc: register upper handler failed\n");
    }
    if (icc_register_handler(SHMEM_EP_RTOS_BENCH, icc_bench_handler) != 0) {
        debug_log("icc: register bench handler failed\n");
    }
#if QUARDAMP_ACCEL_CLIENT
    if (icc_register_handler(SHMEM_EP_RTOS_ACCEL,
                             icc_accel_complete_handler) != 0) {
        debug_log("icc: register accel handler failed\n");
    }
#endif

    /*
     * 创建 UART RX 队列：64 个 char 元素。
     * 必须在 ns16550_enable_rx_irq() 之前创建，
     * 否则 ISR 触发时 xUartRxQueue 仍为 NULL，
     * 字符会被 drain 但不投递。
     */
    xUartRxQueue = xQueueCreate(64, sizeof(char));

    ns16550_enable_rx_irq();

    xTaskCreate(vTaskCreate,"task creat",256,NULL,4,NULL);

	vTaskStartScheduler();
	return 0;
}
