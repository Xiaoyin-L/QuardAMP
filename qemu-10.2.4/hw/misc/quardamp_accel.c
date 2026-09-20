#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
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

#define QACC_MAGIC          0x51414343U /* QACC */
#define QACC_STATUS_DONE    0x1U
#define QACC_STATUS_BUSY    0x2U
#define QACC_CMD_RUN        0x1U
#define QACC_CMD_IRQ        0x2U
#define QACC_IRQ_DONE       0x1U
#define QACC_MAX_XFER       4096U

struct QuardAmpAccelState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    uint32_t status;
    uint32_t irq_status;
    uint64_t src;
    uint64_t dst;
    uint32_t len;
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

static void qacc_run(QuardAmpAccelState *s, uint32_t cmd)
{
    uint8_t buf[QACC_MAX_XFER];
    uint32_t len = s->len;

    if (len > QACC_MAX_XFER) {
        len = QACC_MAX_XFER;
    }

    s->status = QACC_STATUS_BUSY;
    if (len) {
        pci_dma_read(&s->parent_obj, s->src, buf, len);
        for (uint32_t i = 0; i < len; i++) {
            buf[i] ^= 0x5a;
        }
        pci_dma_write(&s->parent_obj, s->dst, buf, len);
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
