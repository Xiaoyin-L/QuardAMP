#ifndef XV6_IOMMU_H
#define XV6_IOMMU_H

#include "types.h"

#define IOMMU_PERM_READ  0x1U
#define IOMMU_PERM_WRITE 0x2U

void iommu_init(void);
void iommu_map_window(uint64 iova, uint64 pa, uint64 len, uint32 perm);
void iommu_enable(void);
void iommu_disable(void);
uint32 iommu_status(void);
uint64 iommu_fault_addr(void);
void iommu_clear_fault(void);

#endif
