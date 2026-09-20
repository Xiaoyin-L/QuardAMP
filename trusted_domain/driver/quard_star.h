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

#define APLIC_S_ADDR 0x0d000000UL
#define APLIC_DOMAINCFG 0x0000UL
#define APLIC_SOURCECFG_BASE 0x0004UL
#define APLIC_SETIENUM 0x1edcUL
#define APLIC_TARGET_BASE 0x3004UL
#define APLIC_DOMAINCFG_IE (1UL << 8)
#define APLIC_DOMAINCFG_DM (1UL << 2)
#define APLIC_SOURCECFG_SM_LEVEL_HIGH 0x6UL
#define APLIC_TARGET_HART_IDX_SHIFT 18

/*
 * AMP mailbox（quardamp-mailbox）doorbell 设备。
 * 地址与 APLIC 中断源编号必须与以下三处保持一致（手工对齐，无 DTS 自动发现）：
 *   - QEMU hw/riscv/quard_star.c 的 virt_memmap[QUARD_STAR_MAILBOX] 与 IRQ enum；
 *   - dts/quard_star_sbi.dts 中 mailbox@10004000 节点的 reg / interrupts；
 *   - xv6 侧 kernel/memlayout.h 的 MAILBOX 定义。
 * 两条 APLIC source 按方向拆分：
 *   MAILBOX_TO_RTOS_IRQ(13): xv6 写 doorbell -> 通知 FreeRTOS(hart7)
 *   MAILBOX_TO_XV6_IRQ (14): FreeRTOS 写 doorbell -> 通知 xv6(hart0~6)
 */
#define MAILBOX_ADDR 0x10004000UL
#define MAILBOX_TO_RTOS_IRQ 13
#define MAILBOX_TO_XV6_IRQ  14

#endif
