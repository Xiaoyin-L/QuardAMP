#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/misc/quardamp_iommu.h"
#include "qemu/module.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(QuardAmpIommuState, QUARDAMP_IOMMU)

#define QIOMMU_REG_CTRL       0x00
#define QIOMMU_REG_STATUS     0x04
#define QIOMMU_REG_IOVA_LO    0x08
#define QIOMMU_REG_IOVA_HI    0x0c
#define QIOMMU_REG_PA_LO      0x10
#define QIOMMU_REG_PA_HI      0x14
#define QIOMMU_REG_MAP_LEN    0x18
#define QIOMMU_REG_MAP_PERM   0x1c
#define QIOMMU_REG_FAULT_LO   0x20
#define QIOMMU_REG_FAULT_HI   0x24

#define QIOMMU_CTRL_ENABLE    0x1U
#define QIOMMU_STATUS_FAULT   0x1U

struct QuardAmpIommuState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t ctrl;
    uint32_t status;
    uint64_t iova_base;
    uint64_t pa_base;
    uint64_t map_len;
    uint32_t map_perm;
    uint64_t fault_addr;
};

static QuardAmpIommuState *quardamp_iommu_singleton;

static void qiommu_record_fault(QuardAmpIommuState *s, uint64_t iova)
{
    s->status |= QIOMMU_STATUS_FAULT;
    s->fault_addr = iova;
}

bool quardamp_iommu_translate(uint64_t iova, uint32_t len,
                              uint32_t perm, uint64_t *pa)
{
    QuardAmpIommuState *s = quardamp_iommu_singleton;
    uint64_t offset;

    if (!s || !(s->ctrl & QIOMMU_CTRL_ENABLE)) {
        *pa = iova;
        return true;
    }

    if ((s->map_perm & perm) != perm || len > s->map_len ||
        iova < s->iova_base) {
        goto fault;
    }

    offset = iova - s->iova_base;
    if (offset > s->map_len || len > s->map_len - offset) {
        goto fault;
    }

    *pa = s->pa_base + offset;
    return true;

fault:
    qiommu_record_fault(s, iova);
    return false;
}

uint32_t quardamp_iommu_status(void)
{
    if (!quardamp_iommu_singleton) {
        return 0;
    }
    return quardamp_iommu_singleton->status;
}

uint64_t quardamp_iommu_fault_addr(void)
{
    if (!quardamp_iommu_singleton) {
        return 0;
    }
    return quardamp_iommu_singleton->fault_addr;
}

static uint64_t qiommu_read(void *opaque, hwaddr addr, unsigned size)
{
    QuardAmpIommuState *s = opaque;

    if (size != 4) {
        return 0xffffffffU;
    }

    switch (addr) {
    case QIOMMU_REG_CTRL:
        return s->ctrl;
    case QIOMMU_REG_STATUS:
        return s->status;
    case QIOMMU_REG_IOVA_LO:
        return (uint32_t)s->iova_base;
    case QIOMMU_REG_IOVA_HI:
        return (uint32_t)(s->iova_base >> 32);
    case QIOMMU_REG_PA_LO:
        return (uint32_t)s->pa_base;
    case QIOMMU_REG_PA_HI:
        return (uint32_t)(s->pa_base >> 32);
    case QIOMMU_REG_MAP_LEN:
        return (uint32_t)s->map_len;
    case QIOMMU_REG_MAP_PERM:
        return s->map_perm;
    case QIOMMU_REG_FAULT_LO:
        return (uint32_t)s->fault_addr;
    case QIOMMU_REG_FAULT_HI:
        return (uint32_t)(s->fault_addr >> 32);
    default:
        return 0;
    }
}

static void qiommu_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    QuardAmpIommuState *s = opaque;

    if (size != 4) {
        return;
    }

    switch (addr) {
    case QIOMMU_REG_CTRL:
        s->ctrl = (uint32_t)val & QIOMMU_CTRL_ENABLE;
        break;
    case QIOMMU_REG_STATUS:
        s->status &= ~(uint32_t)val;
        break;
    case QIOMMU_REG_IOVA_LO:
        s->iova_base = (s->iova_base & 0xffffffff00000000ULL) |
                       (uint32_t)val;
        break;
    case QIOMMU_REG_IOVA_HI:
        s->iova_base = (s->iova_base & 0xffffffffULL) |
                       ((uint64_t)(uint32_t)val << 32);
        break;
    case QIOMMU_REG_PA_LO:
        s->pa_base = (s->pa_base & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case QIOMMU_REG_PA_HI:
        s->pa_base = (s->pa_base & 0xffffffffULL) |
                     ((uint64_t)(uint32_t)val << 32);
        break;
    case QIOMMU_REG_MAP_LEN:
        s->map_len = (uint32_t)val;
        break;
    case QIOMMU_REG_MAP_PERM:
        s->map_perm = (uint32_t)val &
                      (QUARDAMP_IOMMU_PERM_READ | QUARDAMP_IOMMU_PERM_WRITE);
        break;
    }
}

static const MemoryRegionOps qiommu_ops = {
    .read = qiommu_read,
    .write = qiommu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void qiommu_realize(DeviceState *dev, Error **errp)
{
    QuardAmpIommuState *s = QUARDAMP_IOMMU(dev);

    if (quardamp_iommu_singleton) {
        error_setg(errp, "only one quardamp-iommu instance is supported");
        return;
    }

    quardamp_iommu_singleton = s;
}

static void qiommu_instance_init(Object *obj)
{
    QuardAmpIommuState *s = QUARDAMP_IOMMU(obj);

    memory_region_init_io(&s->mmio, obj, &qiommu_ops, s,
                          "quardamp-iommu", 4 * KiB);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void qiommu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qiommu_realize;
}

static const TypeInfo qiommu_info = {
    .name = TYPE_QUARDAMP_IOMMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QuardAmpIommuState),
    .instance_init = qiommu_instance_init,
    .class_init = qiommu_class_init,
};

static void qiommu_register_types(void)
{
    type_register_static(&qiommu_info);
}

type_init(qiommu_register_types)
