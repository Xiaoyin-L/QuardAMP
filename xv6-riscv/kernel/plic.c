#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

//
// RISC-V AIA compatibility layer.
// The public names stay plic_* so the rest of xv6 can migrate gradually.
//

static void
imsic_write(uint64 reg, uint64 val)
{
  w_siselect(reg);
  w_sireg(val);
}

static uint64
imsic_read(uint64 reg)
{
  w_siselect(reg);
  return r_sireg();
}

static void
imsic_enable_irq(int irq)
{
  uint64 mask = 1L << irq;

  imsic_write(IMSIC_EIE0, imsic_read(IMSIC_EIE0) | mask);
}

static void
aplic_setup_source(int irq, int hart)
{
  *(volatile uint32*)APLIC_SOURCECFG(irq) = APLIC_SOURCECFG_SM_LEVEL_HIGH;
  *(volatile uint32*)APLIC_TARGET(irq) =
    ((uint32)hart << APLIC_TARGET_HART_IDX_SHIFT) | (uint32)irq;
  *(volatile uint32*)APLIC_SETIENUM = (uint32)irq;
}

void
plicinit(void)
{
  // Keep shared device interrupts on hart0 for the first AIA bring-up.
  aplic_setup_source(UART0_IRQ, 0);
  aplic_setup_source(VIRTIO0_IRQ, 0);
  aplic_setup_source(MAILBOX_TO_XV6_IRQ, 0);
}

void
plicinithart(void)
{
  int hart = cpuid();

  imsic_write(IMSIC_EITHRESHOLD, 0);
  imsic_write(IMSIC_EIDELIVERY, 1);

  if(hart == 0){
    imsic_enable_irq(UART0_IRQ);
    imsic_enable_irq(VIRTIO0_IRQ);
    imsic_enable_irq(MAILBOX_TO_XV6_IRQ);
    imsic_enable_irq(PCIE_ACCEL_MSI_IRQ);
  }
}

// ask the PLIC what interrupt we should serve.
int
plic_claim(void)
{
  uint64 topei = r_stopei_claim();
  return (int)((topei >> 16) & 0x7ff);
}

// tell the PLIC we've served this IRQ.
void
plic_complete(int irq)
{
  (void)irq;
}
