/*
 * QuardAMP Mailbox Device
 *
 * 该设备为 QuardAMP（xv6 + FreeRTOS）AMP 架构提供跨核 doorbell 通知机制。
 * 它是"mailbox + shared memory + rpmsg"三层核间通信栈的最底层：
 *   - shared memory 承载消息数据；
 *   - 本设备提供"对方何时来读 / 有新消息"的硬件事件通知；
 *   - 上层 rpmsg 端点基于该通知做消息分发。
 *
 * 设计要点：
 *   1. RISC-V 没有标准 mailbox 控制器，故在 quard-star 平台自定义 MMIO 设备；
 *   2. 双向 doorbell 使用两条独立 PLIC 中断源，避免单源被 PLIC 仲裁到单一 context：
 *        - IRQ 线 0 (TX_TO_RTOS): xv6 写 -> 通知 FreeRTOS(hart7)
 *        - IRQ 线 1 (TX_TO_XV6):  FreeRTOS 写 -> 通知 xv6(hart0~6)
 *   3. reason 寄存器可携带低 16 位语义（如"哪条 vring 有数据"），
 *      为上层多 channel 预留扩展能力。
 *
 * Copyright (c) 2026 QuardAMP Project
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_QUARDAMP_MAILBOX_H
#define HW_MISC_QUARDAMP_MAILBOX_H

#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qom/object.h"

#define TYPE_QUARDAMP_MAILBOX "quardamp-mailbox"
OBJECT_DECLARE_SIMPLE_TYPE(QuardampMailboxState, QUARDAMP_MAILBOX)

/*
 * 寄存器布局（相对基址）。
 *
 * TX_TO_RTOS   : xv6 -> FreeRTOS 的 doorbell，写入的值为 reason；
 *                写入后设备将 reason 锁存到 RX_FROM_XV6，并拉起 IRQ 线 0。
 * TX_TO_XV6    : FreeRTOS -> xv6 的 doorbell，写入的值为 reason；
 *                写入后设备将 reason 锁存到 RX_FROM_RTOS，并拉起 IRQ 线 1。
 * RX_FROM_XV6  : FreeRTOS 读取，得到 xv6 最近一次写入的 reason；
 *                写 1 清除 pending 位并撤销 IRQ 线 0。
 * RX_FROM_RTOS : xv6 读取，得到 FreeRTOS 最近一次写入的 reason；
 *                写 1 清除 pending 位并撤销 IRQ 线 1。
 * STATUS       : bit0 = to_rtos pending, bit1 = to_xv6 pending。
 * IRQ_MASK     : bit0 = 使能 to_rtos 中断, bit1 = 使能 to_xv6 中断。
 */
#define QUARDAMP_MAILBOX_TX_TO_RTOS   0x00
#define QUARDAMP_MAILBOX_TX_TO_XV6    0x04
#define QUARDAMP_MAILBOX_RX_FROM_XV6  0x08
#define QUARDAMP_MAILBOX_RX_FROM_RTOS 0x0c
#define QUARDAMP_MAILBOX_STATUS       0x10
#define QUARDAMP_MAILBOX_IRQ_MASK     0x14
#define QUARDAMP_MAILBOX_SIZE         0x20

/* STATUS / IRQ_MASK 位定义 */
#define QUARDAMP_MAILBOX_TO_RTOS_BIT  (1U << 0)
#define QUARDAMP_MAILBOX_TO_XV6_BIT   (1U << 1)

/* IRQ 线编号（设备内部 gpio 输出线） */
#define QUARDAMP_MAILBOX_IRQ_TO_RTOS  0
#define QUARDAMP_MAILBOX_IRQ_TO_XV6   1

struct QuardampMailboxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    /* 两条中断输出线：[0]=to_rtos, [1]=to_xv6 */
    qemu_irq irq[2];

    /* xv6 -> FreeRTOS 方向：xv6 写入的 reason，供 FreeRTOS 读取 */
    uint32_t rx_from_xv6;
    /* FreeRTOS -> xv6 方向：FreeRTOS 写入的 reason，供 xv6 读取 */
    uint32_t rx_from_rtos;
    /* pending 状态位，bit0=to_rtos, bit1=to_xv6 */
    uint32_t status;
    /* 中断使能掩码，bit0=to_rtos, bit1=to_xv6，复位默认全开 */
    uint32_t irq_mask;
};

#endif /* HW_MISC_QUARDAMP_MAILBOX_H */