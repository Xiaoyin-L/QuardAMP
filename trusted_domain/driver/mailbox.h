#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdint.h>

/*
 * QuardAMP mailbox（quardamp-mailbox）doorbell 驱动 —— FreeRTOS(hart7) 侧。
 *
 * 本阶段（阶段 1）只做单向验证：xv6 -> FreeRTOS。
 *   - 发送方：xv6 写 TX_TO_RTOS(0x00)，设备锁存 reason 并拉起 PLIC 源 13；
 *   - 接收方：FreeRTOS 在中断里读 RX_FROM_XV6(0x08) 取 reason，
 *             再向同一寄存器写 1（W1C）清 pending 并撤中断。
 *
 * 寄存器语义（与 QEMU hw/misc/quardamp_mailbox.c 严格对应）：
 *   TX 写触发 / RX 读不清 / W1C ack。
 * FreeRTOS -> xv6 方向（TX_TO_XV6 / RX_FROM_RTOS）在阶段 2 使用，
 * 本文件一并给出寄存器定义，避免阶段 2 重复改头文件。
 */

/* 寄存器偏移（相对 MAILBOX_ADDR 0x10004000） */
#define MAILBOX_TX_TO_RTOS   0x00UL  /* xv6 -> FreeRTOS doorbell（xv6 写） */
#define MAILBOX_TX_TO_XV6    0x04UL  /* FreeRTOS -> xv6 doorbell（阶段 2） */
#define MAILBOX_RX_FROM_XV6  0x08UL  /* FreeRTOS 读 reason / 写 1 ack */
#define MAILBOX_RX_FROM_RTOS 0x0cUL  /* xv6 读 reason / 写 1 ack（阶段 2） */
#define MAILBOX_STATUS       0x10UL  /* bit0=to_rtos pending, bit1=to_xv6 pending */
#define MAILBOX_IRQ_MASK     0x14UL  /* bit0/bit1 分别使能两方向中断 */

/* STATUS / IRQ_MASK 位定义 */
#define MAILBOX_TO_RTOS_BIT  (1U << 0)
#define MAILBOX_TO_XV6_BIT   (1U << 1)

/*
 * 初始化 mailbox 接收方向（xv6 -> FreeRTOS）：
 *   - 设置 PLIC 源 MAILBOX_TO_RTOS_IRQ(13) 的优先级为 1；
 *   - 在 hart7 的 S-mode PLIC context 使能该源。
 * 必须在 vTaskStartScheduler()（打开 sie.SEIE）之前调用，
 * 与 plic_init_hart7() / UART2 的初始化顺序保持一致。
 */
void mailbox_init(void);

/*
 * 读取 xv6 最近一次 doorbell 写入的 reason。
 * 只读不清：设备侧 pending 位保持，直到显式 ack。
 */
uint32_t mailbox_read_rx_from_xv6(void);

/*
 * 应答（ack）xv6 -> FreeRTOS 方向的中断：
 * 向 RX_FROM_XV6 写 1（W1C），设备清除 to_rtos pending 位并撤销 IRQ 线 0。
 * 必须在读取 reason 之后调用，且必须在 plic_complete_hart7() 之前完成，
 * 否则 PLIC complete 后设备侧中断条件仍在，会立即再次 pending 造成中断风暴。
 */
void mailbox_ack_to_rtos(void);

#endif /* MAILBOX_H */