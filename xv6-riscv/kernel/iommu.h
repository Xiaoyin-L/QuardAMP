#ifndef XV6_IOMMU_H
#define XV6_IOMMU_H

#include "types.h"

#define IOMMU_PERM_READ  0x1U
#define IOMMU_PERM_WRITE 0x2U

void iommu_init(void);
void iommu_map_window(uint64 iova, uint64 pa, uint64 len, uint32 perm);
void iommu_select_domain(uint32 domain);
void iommu_map_domain_window(uint32 domain, uint64 iova, uint64 pa,
                             uint64 len, uint32 perm);
void iommu_enable(void);
void iommu_disable(void);
uint32 iommu_status(void);
uint32 iommu_domain_status(uint32 domain);
uint64 iommu_fault_addr(void);
uint64 iommu_domain_fault_addr(uint32 domain);
void iommu_clear_fault(void);
void iommu_clear_domain_fault(uint32 domain);

#endif
