#include <FreeRTOS.h>
#include <task.h>
#include "debug_log.h"
#include "sbi.h"
#include "riscv_asm.h"
#include "ns16550.h"
#include "plic.h"
#include "quard_star.h"

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

static void vTaskCreate(void *p_arg)
{ 
	debug_log("vTaskCreate\n");

    xTaskCreate(task1,"task1",2048,NULL,4,NULL);
    xTaskCreate(task2,"task2",2048,NULL,4,NULL);

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
    ns16550_enable_rx_irq();

    xTaskCreate(vTaskCreate,"task creat",256,NULL,4,NULL);

	vTaskStartScheduler();
	return 0;
}
