#include "types.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "iommu.h"

#define IOMMU_REG_CTRL       0x00
#define IOMMU_REG_STATUS     0x04
#define IOMMU_REG_IOVA_LO    0x08
#define IOMMU_REG_IOVA_HI    0x0c
#define IOMMU_REG_PA_LO      0x10
#define IOMMU_REG_PA_HI      0x14
#define IOMMU_REG_MAP_LEN    0x18
#define IOMMU_REG_MAP_PERM   0x1c
#define IOMMU_REG_FAULT_LO   0x20
#define IOMMU_REG_FAULT_HI   0x24
#define IOMMU_REG_DOMAIN_SEL 0x28

#define IOMMU_CTRL_ENABLE    0x1U
#define IOMMU_STATUS_FAULT   0x1U
#define IOMMU_DOMAIN_COUNT   2U

static volatile uint32*
iommu_reg(uint32 off)
{
  return (volatile uint32*)(QUARD_IOMMU + off);
}

void
iommu_init(void)
{
  iommu_disable();
  for(uint32 domain = 0; domain < IOMMU_DOMAIN_COUNT; domain++)
    iommu_clear_domain_fault(domain);
  printf("iommu: mmio=0x%lx domains=%d\n", (uint64)QUARD_IOMMU,
         IOMMU_DOMAIN_COUNT);
}

void
iommu_select_domain(uint32 domain)
{
  *iommu_reg(IOMMU_REG_DOMAIN_SEL) = domain % IOMMU_DOMAIN_COUNT;
  __sync_synchronize();
}

void
iommu_map_domain_window(uint32 domain, uint64 iova, uint64 pa,
                        uint64 len, uint32 perm)
{
  iommu_select_domain(domain);
  *iommu_reg(IOMMU_REG_IOVA_LO) = (uint32)iova;
  *iommu_reg(IOMMU_REG_IOVA_HI) = (uint32)(iova >> 32);
  *iommu_reg(IOMMU_REG_PA_LO) = (uint32)pa;
  *iommu_reg(IOMMU_REG_PA_HI) = (uint32)(pa >> 32);
  *iommu_reg(IOMMU_REG_MAP_LEN) = (uint32)len;
  *iommu_reg(IOMMU_REG_MAP_PERM) = perm & (IOMMU_PERM_READ | IOMMU_PERM_WRITE);
  __sync_synchronize();
}

void
iommu_map_window(uint64 iova, uint64 pa, uint64 len, uint32 perm)
{
  iommu_map_domain_window(0, iova, pa, len, perm);
}

void
iommu_enable(void)
{
  __sync_synchronize();
  *iommu_reg(IOMMU_REG_CTRL) = IOMMU_CTRL_ENABLE;
}

void
iommu_disable(void)
{
  *iommu_reg(IOMMU_REG_CTRL) = 0;
  __sync_synchronize();
}

uint32
iommu_status(void)
{
  iommu_select_domain(0);
  return *iommu_reg(IOMMU_REG_STATUS);
}

uint32
iommu_domain_status(uint32 domain)
{
  iommu_select_domain(domain);
  return *iommu_reg(IOMMU_REG_STATUS);
}

uint64
iommu_fault_addr(void)
{
  iommu_select_domain(0);
  uint64 lo = *iommu_reg(IOMMU_REG_FAULT_LO);
  uint64 hi = *iommu_reg(IOMMU_REG_FAULT_HI);

  return lo | (hi << 32);
}

uint64
iommu_domain_fault_addr(uint32 domain)
{
  iommu_select_domain(domain);
  uint64 lo = *iommu_reg(IOMMU_REG_FAULT_LO);
  uint64 hi = *iommu_reg(IOMMU_REG_FAULT_HI);

  return lo | (hi << 32);
}

void
iommu_clear_fault(void)
{
  iommu_clear_domain_fault(0);
}

void
iommu_clear_domain_fault(uint32 domain)
{
  iommu_select_domain(domain);
  *iommu_reg(IOMMU_REG_STATUS) = IOMMU_STATUS_FAULT;
}
