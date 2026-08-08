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

#ifndef QUARDAMP_DEMO
#define QUARDAMP_DEMO 0
#endif

#if QUARDAMP_DEMO
static void task1(void *p_arg)
{ 
    int time = 0;
    for(;;)
    {
        debug_log("task1 %x\n",time++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void task2(void *p_arg)
{ 
    int time = 0;
    for(;;)
    {
        debug_log("task2 %x\n",time++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

/*
 * UART RX 消费任务。
 * 阻塞在 xQueueReceive() 上等待 ISR 投递的字符。
 * 当 uart2 收到输入时：
 *   ISR handle_interrupt() -> xQueueSendFromISR()
 *   -> 唤醒本任务 -> portYIELD_FROM_ISR() 触发切换
 *   -> 本任务在任务上下文打印字符
 *
 * 优先级 5，高于 task1/task2 的 4，保证
 * ISR 后立即处理 RX 数据。
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

/*
 * 阶段 2：反向 doorbell（FreeRTOS -> xv6）测试任务。
 * FreeRTOS 侧没有 shell，无法像阶段 1 那样从用户态手动触发，
 * 改用专用任务自动触发（与阶段 1 的 mailboxtest 用户程序对称）。
 *
 * 时序考量：先等 5 秒让 xv6 完成启动并执行 plicinit/plicinithart
 * （在 PLIC 使能源 14）；随后间隔 2 秒连发两次不同 reason，
 * 验证重复触发与 reason 锁存都正确。即便提前触发，
 * 设备 pending 位会保持，xv6 使能后 PLIC 仍会补投递，不会丢。
 */
#if QUARDAMP_DEMO
static void vMailboxTestTask(void *p_arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    debug_log("mailboxtest: ring doorbell to xv6, reason=%x\n",
              (unsigned long)0xa55a);
    mailbox_ring_to_xv6(0xa55a);

    vTaskDelay(pdMS_TO_TICKS(2000));
    debug_log("mailboxtest: ring doorbell to xv6, reason=%x\n",
              (unsigned long)0x002b);
    mailbox_ring_to_xv6(0x002b);

    vTaskDelete(NULL);
}
#endif

static void vTaskCreate(void *p_arg)
{ 
	debug_log("vTaskCreate\n");

#if QUARDAMP_DEMO
    xTaskCreate(task1,"task1",2048,NULL,4,NULL);
    xTaskCreate(task2,"task2",2048,NULL,4,NULL);
#endif
    xTaskCreate(vUartRxTask,"vUartRxTask",2048,NULL,5,NULL);
    xTaskCreate(vIccDispatchTask,"vIccDispatchTask",512,NULL,5,NULL);
#if QUARDAMP_DEMO
    xTaskCreate(vIccTestTask,"vIccTestTask",512,NULL,4,NULL);
    xTaskCreate(vMailboxTestTask,"vMailboxTestTask",512,NULL,4,NULL);
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
     * Stage 6: register a second service endpoint used by xv6 rpctest.
     * The handler runs in vIccDispatchTask context, not in the mailbox ISR,
     * so doing byte-wise payload conversion and sending a reply is safe here.
     */
    if (icc_register_handler(SHMEM_EP_RTOS_UPPER, icc_upper_handler) != 0) {
        debug_log("icc: register upper handler failed\n");
    }
    if (icc_register_handler(SHMEM_EP_RTOS_BENCH, icc_bench_handler) != 0) {
        debug_log("icc: register bench handler failed\n");
    }

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
