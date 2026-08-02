#include <stdint.h>
#include "plic.h"
#include "quard_star.h"

/*
 * QEMU virt / SiFive PLIC 常见 context 编号：
 *
 *   hart0 M-mode = 0
 *   hart0 S-mode = 1
 *   hart1 M-mode = 2
 *   hart1 S-mode = 3
 *   ...
 *
 * 所以 hart7 S-mode = 7 * 2 + 1 = 15。
 *
 * 这里不直接写 15，而是从 PRIM_HART 推导，避免以后调整 trusted hart
 * 时忘记同步 context 编号。
 */
#define PLIC_S_CONTEXT(hart) ((hart) * 2UL + 1UL)
#define PLIC_HART7_S_CONTEXT PLIC_S_CONTEXT(PRIM_HART)

/*
 * priority 寄存器：
 * - 每个 IRQ source 一个 32-bit priority
 * - priority = 0 表示该 source 被 PLIC 层面禁用
 * - priority > threshold 才能投递给目标 context
 */
#define PLIC_PRIORITY(irq) \
    (PLIC_PRIORITY_BASE + ((unsigned long)(irq) * 4UL))

/*
 * enable 寄存器：
 * - 每个 context 有一组 enable bits
 * - 每 32 个 IRQ source 占用一个 32-bit enable word
 * - context 之间的 stride 是 0x80
 */
#define PLIC_ENABLE(context, irq) \
    (PLIC_ENABLE_BASE + ((context) * PLIC_ENABLE_STRIDE) + \
     (((unsigned long)(irq) / 32UL) * 4UL))

/*
 * threshold 寄存器：
 * - priority 必须大于 threshold 才会被投递
 * - 设置为 0 表示允许所有 priority >= 1 的中断进来
 */
#define PLIC_THRESHOLD(context) \
    (PLIC_CONTEXT_BASE + ((context) * PLIC_CONTEXT_STRIDE))

 /*
 * claim/complete 是同一个寄存器：
 * - 读：claim 一个当前最高优先级 pending IRQ
 * - 写：complete 对应 IRQ
 */
#define PLIC_CLAIM(context) \
    (PLIC_CONTEXT_BASE + ((context) * PLIC_CONTEXT_STRIDE) + 4UL)

/*
 * PLIC MMIO 寄存器是 32-bit 宽度。
 *
 * 使用 volatile 的原因：
 * - 这些地址不是普通内存，而是设备寄存器。
 * - 编译器不能缓存读写，也不能把看似无用的读写优化掉。
 */
static inline volatile uint32_t *plic_reg(unsigned long offset)
{
    return (volatile uint32_t *)(PLIC_ADDR + offset);
}

static void plic_set_priority(int irq, uint32_t priority)
{
    if (irq <= 0) {
        return;
    }

    *plic_reg(PLIC_PRIORITY(irq)) = priority;
}

void plic_init_hart7(void)
{
    const unsigned long context = PLIC_HART7_S_CONTEXT;

    /*
     * threshold = 0：
     * 只要某个 IRQ source 的 priority 设置为 1 或更高，就可以投递。
     */
    *plic_reg(PLIC_THRESHOLD(context)) = 0;

    /*
     * 先把 UART2 的 priority 设为 1。
     *
     * 更完整的版本可以在这里统一初始化所有 trusted-domain 使用的 IRQ。
     * 当前阶段只验证 uart2，所以保持最小。
     */
    plic_set_priority(UART2_IRQ, 1);
}

void plic_enable_irq_hart7(int irq)
{
    const unsigned long context = PLIC_HART7_S_CONTEXT;
    volatile uint32_t *enable;
    uint32_t bit;

    if (irq <= 0) {
        return;
    }

    enable = plic_reg(PLIC_ENABLE(context, irq));
    bit = (uint32_t)(1UL << ((unsigned long)irq % 32UL));
    *enable = *enable | bit;
}

int plic_claim_hart7(void)
{
    const unsigned long context = PLIC_HART7_S_CONTEXT;

    return (int)(*plic_reg(PLIC_CLAIM(context)));
}

void plic_complete_hart7(int irq)
{
    const unsigned long context = PLIC_HART7_S_CONTEXT;

    if (irq <= 0) {
        return;
    }

    *plic_reg(PLIC_CLAIM(context)) = (uint32_t)irq;
}
