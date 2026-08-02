/* 负责平台中断分发 */
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
void handle_interrupt(void)
{
    int irq = plic_claim_hart7();

    if (irq == 0) {
        return;
    }

    /*
     * 第一阶段先只打印和 complete，确认中断链路能跑通。
     *
     * 注意：
     * debug_log 是 polling UART 输出。短日志可以用于 bring-up，
     * 但正式 ISR 中不建议大量打印，否则会拉长中断处理时间。
     */
    if (irq == UART2_IRQ) {
        debug_log("plic: uart2 irq %d\n", irq);
        while (ns16550_getc_nonblock() >= 0) {
        }
    } else {
        debug_log("plic: unknown irq %d\n", irq);
    }

    plic_complete_hart7(irq);
}
