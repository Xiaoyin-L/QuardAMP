/* 负责平台中断分发 */
#include <FreeRTOS.h>
#include <queue.h>
#include "debug_log.h"
#include "ns16550.h"
#include "plic.h"
#include "quard_star.h"

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
