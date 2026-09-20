#ifndef HW_MISC_QUARDAMP_IOMMU_H
#define HW_MISC_QUARDAMP_IOMMU_H

#include "qemu/osdep.h"

#define TYPE_QUARDAMP_IOMMU "quardamp-iommu"

#define QUARDAMP_IOMMU_PERM_READ  0x1U
#define QUARDAMP_IOMMU_PERM_WRITE 0x2U

bool quardamp_iommu_translate(uint64_t iova, uint32_t len,
                              uint32_t perm, uint64_t *pa);
uint32_t quardamp_iommu_status(void);
uint64_t quardamp_iommu_fault_addr(void);

#endif
