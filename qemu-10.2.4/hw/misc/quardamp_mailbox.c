/*
 * QuardAMP Mailbox Device
 *
 * 提供 xv6 <-> FreeRTOS 之间的 doorbell 通知。实现思路见
 * include/hw/misc/quardamp_mailbox.h 顶部注释。
 *
 * 中断触发模型（与 PLIC 配合）：
 *   - 设备 IRQ 线 0 连接到 PLIC 源 QUARD_STAR_MAILBOX_TO_RTOS_IRQ，
 *     该源由 FreeRTOS(hart7) 在其 PLIC context 中使能；
 *   - 设备 IRQ 线 1 连接到 PLIC 源 QUARD_STAR_MAILBOX_TO_XV6_IRQ，
 *     该源由 xv6(hart0~6) 在其 PLIC context 中使能。
 *   PLIC 会根据各 context 的 enable 位把中断投递给对应 hart。
 *
 * Copyright (c) 2026 QuardAMP Project
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/misc/quardamp_mailbox.h"

/*
 * 更新某一条 IRQ 线的电平。
 * pending 且对应 mask 打开 -> 拉高中断；否则拉低。
 */
static void quardamp_mailbox_update_irq(QuardampMailboxState *s, uint32_t bit,
                                        int line)
{
    bool level = (s->status & bit) && (s->irq_mask & bit);

    qemu_set_irq(s->irq[line], level);
}

static uint64_t quardamp_mailbox_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    QuardampMailboxState *s = QUARDAMP_MAILBOX(opaque);

    switch (addr) {
    case QUARDAMP_MAILBOX_RX_FROM_XV6:
        /* FreeRTOS 读取 xv6 写入的 reason；只读不清，ack 由写操作完成 */
        return s->rx_from_xv6;

    case QUARDAMP_MAILBOX_RX_FROM_RTOS:
        /* xv6 读取 FreeRTOS 写入的 reason；只读不清 */
        return s->rx_from_rtos;

    case QUARDAMP_MAILBOX_STATUS:
        return s->status;

    case QUARDAMP_MAILBOX_IRQ_MASK:
        return s->irq_mask;

    case QUARDAMP_MAILBOX_TX_TO_RTOS:
    case QUARDAMP_MAILBOX_TX_TO_XV6:
        /* TX 寄存器为"只写 doorbell"，读无意义，返回 0 */
        return 0;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void quardamp_mailbox_write(void *opaque, hwaddr addr,
                                   uint64_t val64, unsigned int size)
{
    QuardampMailboxState *s = QUARDAMP_MAILBOX(opaque);
    uint32_t val = val64;

    switch (addr) {
    case QUARDAMP_MAILBOX_TX_TO_RTOS:
        /*
         * xv6 -> FreeRTOS doorbell。
         * 锁存 reason，置 pending 位，触发 IRQ 线 0。
         */
        s->rx_from_xv6 = val;
        s->status |= QUARDAMP_MAILBOX_TO_RTOS_BIT;
        quardamp_mailbox_update_irq(s, QUARDAMP_MAILBOX_TO_RTOS_BIT,
                                    QUARDAMP_MAILBOX_IRQ_TO_RTOS);
        break;

    case QUARDAMP_MAILBOX_TX_TO_XV6:
        /*
         * FreeRTOS -> xv6 doorbell。
         * 锁存 reason，置 pending 位，触发 IRQ 线 1。
         */
        s->rx_from_rtos = val;
        s->status |= QUARDAMP_MAILBOX_TO_XV6_BIT;
        quardamp_mailbox_update_irq(s, QUARDAMP_MAILBOX_TO_XV6_BIT,
                                    QUARDAMP_MAILBOX_IRQ_TO_XV6);
        break;

    case QUARDAMP_MAILBOX_RX_FROM_XV6:
        /*
         * FreeRTOS ack：写 1 清除 to_rtos pending 位并撤销 IRQ 线 0。
         * 采用 W1C（write-1-to-clear）语义，避免误清另一方向的状态。
         */
        if (val & QUARDAMP_MAILBOX_TO_RTOS_BIT) {
            s->status &= ~QUARDAMP_MAILBOX_TO_RTOS_BIT;
            quardamp_mailbox_update_irq(s, QUARDAMP_MAILBOX_TO_RTOS_BIT,
                                        QUARDAMP_MAILBOX_IRQ_TO_RTOS);
        }
        break;

    case QUARDAMP_MAILBOX_RX_FROM_RTOS:
        /*
         * xv6 ack：写 1 清除 to_xv6 pending 位并撤销 IRQ 线 1。
         */
        if (val & QUARDAMP_MAILBOX_TO_XV6_BIT) {
            s->status &= ~QUARDAMP_MAILBOX_TO_XV6_BIT;
            quardamp_mailbox_update_irq(s, QUARDAMP_MAILBOX_TO_XV6_BIT,
                                        QUARDAMP_MAILBOX_IRQ_TO_XV6);
        }
        break;

    case QUARDAMP_MAILBOX_IRQ_MASK:
        /* 更新中断使能掩码，并同步刷新两条 IRQ 线电平 */
        s->irq_mask = val;
        quardamp_mailbox_update_irq(s, QUARDAMP_MAILBOX_TO_RTOS_BIT,
                                    QUARDAMP_MAILBOX_IRQ_TO_RTOS);
        quardamp_mailbox_update_irq(s, QUARDAMP_MAILBOX_TO_XV6_BIT,
                                    QUARDAMP_MAILBOX_IRQ_TO_XV6);
        break;

    case QUARDAMP_MAILBOX_STATUS:
        /* STATUS 只读，忽略写入 */
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps quardamp_mailbox_ops = {
    .read = quardamp_mailbox_read,
    .write = quardamp_mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const VMStateDescription quardamp_mailbox_vmstate = {
    .name = TYPE_QUARDAMP_MAILBOX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(rx_from_xv6, QuardampMailboxState),
        VMSTATE_UINT32(rx_from_rtos, QuardampMailboxState),
        VMSTATE_UINT32(status, QuardampMailboxState),
        VMSTATE_UINT32(irq_mask, QuardampMailboxState),
        VMSTATE_END_OF_LIST()
    }
};

static void quardamp_mailbox_reset_hold(Object *obj, ResetType type)
{
    QuardampMailboxState *s = QUARDAMP_MAILBOX(obj);

    s->rx_from_xv6 = 0;
    s->rx_from_rtos = 0;
    s->status = 0;
    /* 复位默认两个方向中断都使能，简化上层初始化 */
    s->irq_mask = QUARDAMP_MAILBOX_TO_RTOS_BIT | QUARDAMP_MAILBOX_TO_XV6_BIT;
}

static void quardamp_mailbox_init(Object *obj)
{
    QuardampMailboxState *s = QUARDAMP_MAILBOX(obj);

    /* 注册 MMIO 区域 */
    memory_region_init_io(&s->mmio, obj, &quardamp_mailbox_ops, s,
                          TYPE_QUARDAMP_MAILBOX, QUARDAMP_MAILBOX_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    /*
     * 两条中断输出线（sysbus IRQ，供 sysbus_connect_irq 连接到 PLIC）：
     *   线 0 -> PLIC 源，通知 FreeRTOS
     *   线 1 -> PLIC 源，通知 xv6
     *
     * 注意：必须用 sysbus_init_irq 而不是 qdev_init_gpio_out_named，
     * 否则板级 sysbus_connect_irq 会找不到 "sysbus-irq[n]" 属性。
     */
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[0]);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[1]);
}

static void quardamp_mailbox_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = quardamp_mailbox_reset_hold;
    dc->vmsd = &quardamp_mailbox_vmstate;
    dc->desc = "QuardAMP mailbox doorbell device";
}

static const TypeInfo quardamp_mailbox_info = {
    .name          = TYPE_QUARDAMP_MAILBOX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QuardampMailboxState),
    .instance_init = quardamp_mailbox_init,
    .class_init    = quardamp_mailbox_class_init,
};

static void quardamp_mailbox_register_types(void)
{
    type_register_static(&quardamp_mailbox_info);
}

type_init(quardamp_mailbox_register_types)