/* 负责平台中断分发 */
#include <FreeRTOS.h>
#include <queue.h>
#include "debug_log.h"
#include "ns16550.h"
#include "plic.h"
#include "quard_star.h"
#include "mailbox.h"
#include "shmem.h"
#include "icc.h"

/*
 * FreeRTOS RISC-V portASM.S 中声明了 weak handle_interrupt。
 *
 * 这里提供同名强符号后，链接器会选择本函数。
 * 当 scause 是 Supervisor external interrupt 时，portASM.S 会跳到这里。
 */

 /*
 * UART RX 队列句柄。
 * 由 main.c 在 vTaskStartScheduler() 之前通过 xQueueCreate() 创建。
 * ISR 向此队列投递从 uart2 RX FIFO 读到的字符；
 * 消费任务 vUartRxTask 阻塞在 xQueueReceive() 上等待数据。
 */
QueueHandle_t xUartRxQueue = NULL;


void handle_interrupt(void)
{
    int irq = plic_claim_hart7();

    if (irq == 0) {
        return;
    }

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (irq == UART2_IRQ) {
        /*
         * 读掉 UART RX FIFO 中的全部字符。
         * 必须在 complete 之前读完，否则 UART 设备侧
         * 中断条件仍然存在，PLIC complete 后会立即
         * 再次 pending，导致中断风暴。
         */
        int ch;
        while ((ch = ns16550_getc_nonblock()) >= 0) {
            if (xUartRxQueue != NULL) {
                char c = (char)ch;
                xQueueSendFromISR(
                    xUartRxQueue, &c,
                    &xHigherPriorityTaskWoken);
            }
        }
    } else if (irq == MAILBOX_TO_RTOS_IRQ) {
        /*
        * 阶段 1 单向 doorbell（xv6 -> FreeRTOS）冒烟处理：
        * 顺序必须是 读 reason -> W1C ack -> PLIC complete：
        *   1. 先读 RX_FROM_XV6 拿到 reason（只读不清，不丢 pending）；
        *   2. 再向 RX_FROM_XV6 写 1 清设备侧 pending 并撤 IRQ 线，
        *      若把 ack 放到 PLIC complete 之后，complete 瞬间设备侧
        *      中断条件仍在，会立即再次 pending 造成中断风暴；
        *   3. 最后由函数末尾的 plic_complete_hart7() 完成 PLIC 侧应答。
        * 已知限制：读与 ack 之间若 xv6 又写一次 doorbell，新 reason
        * 会覆盖旧值且被本次 ack 一并清掉（设备 reason 是直接赋值锁存）。
        * 单向冒烟可接受；阶段 3 引入 shared memory 后，真实消息状态
        * 以共享内存为准，doorbell 退化为纯通知。
        */
        uint32_t reason = mailbox_read_rx_from_xv6();
        mailbox_ack_to_rtos();
        debug_log("mailbox irq received, reason=%x\n",
                (unsigned long)reason);

        if (reason == SHMEM_DOORBELL_CH0) {
            icc_isr_drain_to_rtos();
        }
    } else {
        debug_log("plic: unknown irq %d\n", irq);
    }

    plic_complete_hart7(irq);

    /*
     * 如果 xQueueSendFromISR 唤醒了比当前被中断任务
     * 更高优先级的任务（vUartRxTask 优先级 5 >
     * task1/task2 优先级 4），则在退出 ISR 时触发
     * 一次任务切换，让消费任务立即运行。
     */
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
