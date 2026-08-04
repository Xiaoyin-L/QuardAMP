#ifndef QUARD_STAR_H
#define QUARD_STAR_H

#define PRIM_HART 7UL

#define UART0_ADDR 0x10000000UL
#define UART1_ADDR 0x10003000UL
#define UART2_ADDR 0x10002000UL

#define UART0_IRQ 10
#define UART1_IRQ 11
#define UART2_IRQ 12

#define CLINT_ADDR 0x02000000UL
#define CLINT_MTIMECMP 0x4000UL
#define CLINT_MTIME 0xbff8UL

#define PLIC_ADDR 0x0c000000UL
#define PLIC_PRIORITY_BASE 0x000000UL
#define PLIC_ENABLE_BASE 0x002000UL
#define PLIC_CONTEXT_BASE 0x200000UL
#define PLIC_ENABLE_STRIDE 0x80UL
#define PLIC_CONTEXT_STRIDE 0x1000UL

/*
 * AMP mailbox（quardamp-mailbox）doorbell 设备。
 * 地址与 PLIC 中断源编号必须与以下三处保持一致（手工对齐，无 DTS 自动发现）：
 *   - QEMU hw/riscv/quard_star.c 的 virt_memmap[QUARD_STAR_MAILBOX] 与 IRQ enum；
 *   - dts/quard_star_sbi.dts 中 mailbox@10004000 节点的 reg / interrupts；
 *   - xv6 侧 kernel/memlayout.h 的 MAILBOX 定义。
 * 两条 PLIC 源按方向拆分（避免单源被 PLIC 仲裁到单一 context）：
 *   MAILBOX_TO_RTOS_IRQ(13): xv6 写 doorbell -> 通知 FreeRTOS(hart7)
 *   MAILBOX_TO_XV6_IRQ (14): FreeRTOS 写 doorbell -> 通知 xv6(hart0~6)
 */
#define MAILBOX_ADDR 0x10004000UL
#define MAILBOX_TO_RTOS_IRQ 13
#define MAILBOX_TO_XV6_IRQ  14

#endif
