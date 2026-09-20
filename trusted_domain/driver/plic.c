#include <stdint.h>
#include "plic.h"
#include "quard_star.h"
#include "riscv_asm.h"

/*
 * RISC-V AIA compatibility layer for the existing FreeRTOS code.
 * Device lines enter the S-level APLIC and are delivered as MSIs to
 * hart7's S-level IMSIC.
 */

static inline volatile uint32_t *aplic_reg(unsigned long offset)
{
    return (volatile uint32_t *)(APLIC_S_ADDR + offset);
}

static void imsic_write(unsigned long reg, unsigned long val)
{
    csr_write(CSR_SISELECT, reg);
    csr_write(CSR_SIREG, val);
}

static unsigned long imsic_read(unsigned long reg)
{
    csr_write(CSR_SISELECT, reg);
    return csr_read(CSR_SIREG);
}

static void imsic_enable_irq(int irq)
{
    unsigned long mask = 1UL << (unsigned long)irq;

    imsic_write(IMSIC_EIE0, imsic_read(IMSIC_EIE0) | mask);
}

static void aplic_setup_source(int irq)
{
    *aplic_reg(APLIC_SOURCECFG_BASE + ((unsigned long)(irq - 1) * 4UL)) =
        APLIC_SOURCECFG_SM_LEVEL_HIGH;
    *aplic_reg(APLIC_TARGET_BASE + ((unsigned long)(irq - 1) * 4UL)) =
        ((uint32_t)PRIM_HART << APLIC_TARGET_HART_IDX_SHIFT) |
        (uint32_t)irq;
    *aplic_reg(APLIC_SETIENUM) = (uint32_t)irq;
}

void plic_init_hart7(void)
{
    imsic_write(IMSIC_EITHRESHOLD, 0);
    imsic_write(IMSIC_EIDELIVERY, 1);
    aplic_setup_source(UART2_IRQ);
}

void plic_enable_irq_hart7(int irq)
{
    if (irq <= 0) {
        return;
    }

    aplic_setup_source(irq);
    imsic_enable_irq(irq);
}

int plic_claim_hart7(void)
{
    unsigned long topei;

    __asm__ volatile ("csrrw %0, 0x15c, zero" : "=r"(topei) : : "memory");
    return (int)((topei >> 16) & 0x7ffUL);
}

void plic_complete_hart7(int irq)
{
    (void)irq;
}

void plic_set_irq_priority(int irq, uint32_t priority)
{
    (void)priority;
    if (irq > 0) {
        aplic_setup_source(irq);
    }
}
