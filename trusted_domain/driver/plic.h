#ifndef PLIC_H
#define PLIC_H

/*
 * 初始化 hart7 的 S-mode PLIC context。
 *
 * 这个函数只负责 PLIC 本身：
 * - 设置 hart7 S-mode threshold
 * - 可选清理 enable 寄存器
 *
 * 它不负责打开 CPU 的 sie.SEIE，也不负责初始化 uart2。
 */
void plic_init_hart7(void);

/*
 * 启用某个 PLIC interrupt source。
 *
 * irq 是 PLIC source ID，例如 UART2_IRQ。
 * PLIC 的 source 0 表示 no interrupt，不能作为真实设备 IRQ 使用。
 */
void plic_enable_irq_hart7(int irq);

/*
 * 从 hart7 S-mode claim/complete 寄存器读取当前待处理 IRQ。
 *
 * 返回值：
 * - 0：没有 pending external interrupt
 * - >0：具体 PLIC source ID
 */
int plic_claim_hart7(void);

/*
 * 告诉 PLIC 某个 IRQ 已经处理完成。
 *
 * PLIC 要求把 claim 得到的同一个 IRQ ID 写回 claim/complete 寄存器。
 * 如果不 complete，同一个设备后续可能无法再次触发中断。
 */
void plic_complete_hart7(int irq);

#endif /* PLIC_H */