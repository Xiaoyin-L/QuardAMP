#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/misc/quardamp_iommu.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_QUARDAMP_ACCEL "quardamp-accel"
OBJECT_DECLARE_SIMPLE_TYPE(QuardAmpAccelState, QUARDAMP_ACCEL)

#define QACC_REG_MAGIC      0x00
#define QACC_REG_STATUS     0x04
#define QACC_REG_SRC_LO     0x08
#define QACC_REG_SRC_HI     0x0c
#define QACC_REG_DST_LO     0x10
#define QACC_REG_DST_HI     0x14
#define QACC_REG_LEN        0x18
#define QACC_REG_CMD        0x1c
#define QACC_REG_IRQ_STATUS 0x20
#define QACC_REG_IRQ_ACK    0x24
#define QACC_REG_IOMMU_CTRL 0x28
#define QACC_REG_IOMMU_STATUS 0x2c
#define QACC_REG_IOVA_LO    0x30
#define QACC_REG_IOVA_HI    0x34
#define QACC_REG_PA_LO      0x38
#define QACC_REG_PA_HI      0x3c
#define QACC_REG_MAP_LEN    0x40
#define QACC_REG_MAP_PERM   0x44
#define QACC_REG_FAULT_LO   0x48
#define QACC_REG_FAULT_HI   0x4c

#define QACC_MAGIC          0x51414343U /* QACC */
#define QACC_STATUS_DONE    0x1U
#define QACC_STATUS_BUSY    0x2U
#define QACC_STATUS_FAULT   0x4U
#define QACC_CMD_RUN        0x1U
#define QACC_CMD_IRQ        0x2U
#define QACC_IRQ_DONE       0x1U
#define QACC_MAX_XFER       4096U
#define QACC_IOMMU_ENABLE   0x1U
#define QACC_IOMMU_FAULT    0x1U
#define QACC_MAP_READ       0x1U
#define QACC_MAP_WRITE      0x2U

struct QuardAmpAccelState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    uint32_t status;
    uint32_t irq_status;
    uint64_t src;
    uint64_t dst;
    uint32_t len;
    uint32_t iommu_ctrl;
    uint32_t iommu_status;
    uint64_t iova_base;
    uint64_t pa_base;
    uint64_t map_len;
    uint32_t map_perm;
    uint64_t fault_addr;
};

static void qacc_raise_irq(QuardAmpAccelState *s)
{
    s->irq_status |= QACC_IRQ_DONE;
    if (msi_enabled(&s->parent_obj)) {
        msi_notify(&s->parent_obj, 0);
    } else {
        pci_set_irq(&s->parent_obj, 1);
    }
}

static void qacc_lower_irq(QuardAmpAccelState *s, uint32_t mask)
{
    s->irq_status &= ~mask;
    if (!s->irq_status && !msi_enabled(&s->parent_obj)) {
        pci_set_irq(&s->parent_obj, 0);
    }
}

static bool qacc_translate(QuardAmpAccelState *s, uint64_t iova,
                           uint32_t len, uint32_t perm, uint64_t *pa)
{
    uint32_t qperm = 0;

    if (perm & QACC_MAP_READ) {
        qperm |= QUARDAMP_IOMMU_PERM_READ;
    }
    if (perm & QACC_MAP_WRITE) {
        qperm |= QUARDAMP_IOMMU_PERM_WRITE;
    }

    if (!quardamp_iommu_translate(iova, len, qperm, pa)) {
        s->iommu_status |= QACC_IOMMU_FAULT;
        s->fault_addr = quardamp_iommu_fault_addr();
        return false;
    }
    return true;
}

static void qacc_run(QuardAmpAccelState *s, uint32_t cmd)
{
    uint8_t buf[QACC_MAX_XFER];
    uint32_t len = s->len;
    uint64_t src_pa;
    uint64_t dst_pa;

    if (len > QACC_MAX_XFER) {
        len = QACC_MAX_XFER;
    }

    s->status = QACC_STATUS_BUSY;
    if (len) {
        if (!qacc_translate(s, s->src, len, QACC_MAP_READ, &src_pa) ||
            !qacc_translate(s, s->dst, len, QACC_MAP_WRITE, &dst_pa)) {
            s->status = QACC_STATUS_DONE | QACC_STATUS_FAULT;
            if (cmd & QACC_CMD_IRQ) {
                qacc_raise_irq(s);
            }
            return;
        }

        pci_dma_read(&s->parent_obj, src_pa, buf, len);
        for (uint32_t i = 0; i < len; i++) {
            buf[i] ^= 0x5a;
        }
        pci_dma_write(&s->parent_obj, dst_pa, buf, len);
    }
    s->status = QACC_STATUS_DONE;

    if (cmd & QACC_CMD_IRQ) {
        qacc_raise_irq(s);
    }
}

static uint64_t qacc_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    QuardAmpAccelState *s = opaque;

    if (size != 4) {
        return 0xffffffffU;
    }

    switch (addr) {
    case QACC_REG_MAGIC:
        return QACC_MAGIC;
    case QACC_REG_STATUS:
        return s->status;
    case QACC_REG_SRC_LO:
        return (uint32_t)s->src;
    case QACC_REG_SRC_HI:
        return (uint32_t)(s->src >> 32);
    case QACC_REG_DST_LO:
        return (uint32_t)s->dst;
    case QACC_REG_DST_HI:
        return (uint32_t)(s->dst >> 32);
    case QACC_REG_LEN:
        return s->len;
    case QACC_REG_IRQ_STATUS:
        return s->irq_status;
    case QACC_REG_IOMMU_CTRL:
        return s->iommu_ctrl;
    case QACC_REG_IOMMU_STATUS:
        return s->iommu_status;
    case QACC_REG_IOVA_LO:
        return (uint32_t)s->iova_base;
    case QACC_REG_IOVA_HI:
        return (uint32_t)(s->iova_base >> 32);
    case QACC_REG_PA_LO:
        return (uint32_t)s->pa_base;
    case QACC_REG_PA_HI:
        return (uint32_t)(s->pa_base >> 32);
    case QACC_REG_MAP_LEN:
        return (uint32_t)s->map_len;
    case QACC_REG_MAP_PERM:
        return s->map_perm;
    case QACC_REG_FAULT_LO:
        return (uint32_t)s->fault_addr;
    case QACC_REG_FAULT_HI:
        return (uint32_t)(s->fault_addr >> 32);
    default:
        return 0;
    }
}

static void qacc_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    QuardAmpAccelState *s = opaque;

    if (size != 4) {
        return;
    }

    switch (addr) {
    case QACC_REG_STATUS:
        s->status &= ~(uint32_t)val;
        break;
    case QACC_REG_SRC_LO:
        s->src = (s->src & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case QACC_REG_SRC_HI:
        s->src = (s->src & 0xffffffffULL) | ((uint64_t)(uint32_t)val << 32);
        break;
    case QACC_REG_DST_LO:
        s->dst = (s->dst & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case QACC_REG_DST_HI:
        s->dst = (s->dst & 0xffffffffULL) | ((uint64_t)(uint32_t)val << 32);
        break;
    case QACC_REG_LEN:
        s->len = (uint32_t)val;
        break;
    case QACC_REG_CMD:
        if (val & QACC_CMD_RUN) {
            qacc_run(s, (uint32_t)val);
        }
        break;
    case QACC_REG_IRQ_ACK:
        qacc_lower_irq(s, (uint32_t)val);
        break;
    case QACC_REG_IOMMU_CTRL:
        s->iommu_ctrl = (uint32_t)val & QACC_IOMMU_ENABLE;
        break;
    case QACC_REG_IOMMU_STATUS:
        s->iommu_status &= ~(uint32_t)val;
        if ((val & QACC_IOMMU_FAULT) != 0) {
            s->status &= ~QACC_STATUS_FAULT;
        }
        break;
    case QACC_REG_IOVA_LO:
        s->iova_base = (s->iova_base & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case QACC_REG_IOVA_HI:
        s->iova_base = (s->iova_base & 0xffffffffULL) |
                       ((uint64_t)(uint32_t)val << 32);
        break;
    case QACC_REG_PA_LO:
        s->pa_base = (s->pa_base & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case QACC_REG_PA_HI:
        s->pa_base = (s->pa_base & 0xffffffffULL) |
                     ((uint64_t)(uint32_t)val << 32);
        break;
    case QACC_REG_MAP_LEN:
        s->map_len = (uint32_t)val;
        break;
    case QACC_REG_MAP_PERM:
        s->map_perm = (uint32_t)val & (QACC_MAP_READ | QACC_MAP_WRITE);
        break;
    }
}

static const MemoryRegionOps qacc_bar0_ops = {
    .read = qacc_bar0_read,
    .write = qacc_bar0_write,
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

static void qacc_realize(PCIDevice *pdev, Error **errp)
{
    QuardAmpAccelState *s = QUARDAMP_ACCEL(pdev);

    pci_config_set_interrupt_pin(pdev->config, 1);
    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    memory_region_init_io(&s->bar0, OBJECT(s), &qacc_bar0_ops, s,
                          "quardamp-accel-bar0", 64 * KiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
}

static void qacc_exit(PCIDevice *pdev)
{
    msi_uninit(pdev);
}

static void qacc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = qacc_realize;
    k->exit = qacc_exit;
    k->vendor_id = 0x1efd;
    k->device_id = 0xa001;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo qacc_info = {
    .name = TYPE_QUARDAMP_ACCEL,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(QuardAmpAccelState),
    .class_init = qacc_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void qacc_register_types(void)
{
    type_register_static(&qacc_info);
}

type_init(qacc_register_types)
