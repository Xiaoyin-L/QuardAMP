#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include "debug_log.h"
#include "sbi.h"
#include "riscv_asm.h"
#include "ns16550.h"
#include "plic.h"
#include "quard_star.h"

extern QueueHandle_t xUartRxQueue;

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

static void vTaskCreate(void *p_arg)
{ 
	debug_log("vTaskCreate\n");

    xTaskCreate(task1,"task1",2048,NULL,4,NULL);
    xTaskCreate(task2,"task2",2048,NULL,4,NULL);
    xTaskCreate(vUartRxTask,"vUartRxTask",2048,NULL,5,NULL);

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
