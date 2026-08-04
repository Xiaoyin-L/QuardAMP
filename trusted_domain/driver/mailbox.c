/*
 * QuardAMP mailbox doorbell 驱动实现 —— FreeRTOS(hart7) 侧。
 *
 * 阶段 1 目标：打通单向 doorbell（xv6 -> FreeRTOS）。
 * 本文件只实现接收方向（读 reason + W1C ack）与 PLIC 注册；
 * 发送方向（FreeRTOS -> xv6）留给阶段 2。
 *
 * 硬件行为参照 QEMU hw/misc/quardamp_mailbox.c：
 *   - xv6 写 TX_TO_RTOS：设备锁存 reason 到 RX_FROM_XV6，
 *     置 STATUS.to_rtos pending，拉起设备 IRQ 线 0（PLIC 源 13）；
 *   - FreeRTOS 读 RX_FROM_XV6：只取值，不清 pending；
 *   - FreeRTOS 向 RX_FROM_XV6 写 1：W1C 清 pending 并撤销 IRQ 线 0。
 */
#include <stdint.h>
#include "mailbox.h"
#include "plic.h"
#include "quard_star.h"

/*
 * MMIO 寄存器访问辅助：
 * 地址是设备寄存器而非普通内存，必须 volatile，
 * 防止编译器缓存值或优化掉看似无用的读写。
 */
static inline volatile uint32_t *mailbox_reg(unsigned long offset)
{
    return (volatile uint32_t *)(MAILBOX_ADDR + offset);
}

void mailbox_init(void)
{
    /*
     * PLIC 源 13 优先级设为 1（> hart7 context threshold 0 即可投递）。
     * 不设置优先级时 PLIC 视该 source 为禁用，中断永远不来。
     */
    plic_set_irq_priority(MAILBOX_TO_RTOS_IRQ, 1);

    /*
     * 在 hart7 S-mode context 的 enable 位图里打开源 13。
     * 中断最终是否发生还取决于 sie.SEIE（由 FreeRTOS port
     * 在 vTaskStartScheduler() 时打开），因此这里提前使能是安全的。
     */
    plic_enable_irq_hart7(MAILBOX_TO_RTOS_IRQ);
}

uint32_t mailbox_read_rx_from_xv6(void)
{
    /* 只读不清：pending 位保持到显式 ack */
    return *mailbox_reg(MAILBOX_RX_FROM_XV6);
}

void mailbox_ack_to_rtos(void)
{
    /*
     * W1C：写 1 清除 to_rtos pending 位（bit0），设备撤销 IRQ 线 0。
     * 写 0 无效，避免误清另一方向（to_xv6）的状态。
     */
    *mailbox_reg(MAILBOX_RX_FROM_XV6) = MAILBOX_TO_RTOS_BIT;
}