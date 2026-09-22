#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
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
#define QACC_REG_QUEUE_SEL  0x50
#define QACC_REG_QUEUE_DOMAIN 0x54
#define QACC_REG_QUEUE_VECTOR 0x58
#define QACC_REG_QUEUE_COUNT 0x5c
#define QACC_REG_SQ_BASE_LO  0x60
#define QACC_REG_SQ_BASE_HI  0x64
#define QACC_REG_SQ_SIZE     0x68
#define QACC_REG_SQ_TAIL     0x6c
#define QACC_REG_CQ_BASE_LO  0x70
#define QACC_REG_CQ_BASE_HI  0x74
#define QACC_REG_CQ_SIZE     0x78
#define QACC_REG_CQ_HEAD     0x7c
#define QACC_REG_DOORBELL    0x80
#define QACC_REG_CQ_TAIL     0x84

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
#define QACC_QUEUE_COUNT    2U
#define QACC_MSIX_BAR       4U
#define QACC_DESC_SIZE      32U
#define QACC_CQE_SIZE       16U
#define QACC_OP_XOR         1U

typedef struct QAccDesc {
    uint64_t src;
    uint64_t dst;
    uint32_t len;
    uint16_t opcode;
    uint16_t flags;
    uint32_t job_id;
    uint32_t reserved;
} QEMU_PACKED QAccDesc;

typedef struct QAccCqe {
    uint32_t job_id;
    int32_t status;
    uint32_t len;
    uint32_t flags;
} QEMU_PACKED QAccCqe;

typedef struct QAccQueue {
    uint32_t status;
    uint64_t src;
    uint64_t dst;
    uint32_t len;
    uint32_t domain;
    uint32_t vector;
    uint64_t sq_base;
    uint64_t cq_base;
    uint32_t sq_size;
    uint32_t cq_size;
    uint32_t sq_head;
    uint32_t sq_tail;
    uint32_t cq_head;
    uint32_t cq_tail;
} QAccQueue;

struct QuardAmpAccelState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    uint32_t irq_status;
    uint32_t iommu_ctrl;
    uint32_t iommu_status;
    uint64_t iova_base;
    uint64_t pa_base;
    uint64_t map_len;
    uint32_t map_perm;
    uint64_t fault_addr;
    uint32_t queue_sel;
    QAccQueue queues[QACC_QUEUE_COUNT];
};

static QAccQueue *qacc_selected_queue(QuardAmpAccelState *s)
{
    return &s->queues[s->queue_sel % QACC_QUEUE_COUNT];
}

static void qacc_raise_irq(QuardAmpAccelState *s, uint32_t queue)
{
    QAccQueue *q = &s->queues[queue % QACC_QUEUE_COUNT];

    s->irq_status |= 1U << (queue % QACC_QUEUE_COUNT);
    if (msix_enabled(&s->parent_obj)) {
        msix_notify(&s->parent_obj, q->vector % QACC_QUEUE_COUNT);
    } else if (msi_enabled(&s->parent_obj)) {
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
                           uint32_t len, uint32_t perm, uint32_t domain,
                           uint64_t *pa)
{
    uint32_t qperm = 0;

    if (perm & QACC_MAP_READ) {
        qperm |= QUARDAMP_IOMMU_PERM_READ;
    }
    if (perm & QACC_MAP_WRITE) {
        qperm |= QUARDAMP_IOMMU_PERM_WRITE;
    }

    if (!quardamp_iommu_translate(domain, iova, len, qperm, pa)) {
        s->iommu_status |= QACC_IOMMU_FAULT;
        s->fault_addr = quardamp_iommu_domain_fault_addr(domain);
        return false;
    }
    return true;
}

static int qacc_execute_xor(QuardAmpAccelState *s, QAccQueue *q,
                            uint64_t src, uint64_t dst, uint32_t len)
{
    uint8_t buf[QACC_MAX_XFER];
    uint64_t src_pa;
    uint64_t dst_pa;

    if (len > QACC_MAX_XFER) {
        len = QACC_MAX_XFER;
    }
    if (!len) {
        return 0;
    }
    if (!qacc_translate(s, src, len, QACC_MAP_READ, q->domain, &src_pa) ||
        !qacc_translate(s, dst, len, QACC_MAP_WRITE, q->domain, &dst_pa)) {
        return -1;
    }

    pci_dma_read(&s->parent_obj, src_pa, buf, len);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] ^= 0x5a;
    }
    pci_dma_write(&s->parent_obj, dst_pa, buf, len);
    return 0;
}

static void qacc_run(QuardAmpAccelState *s, uint32_t cmd)
{
    uint32_t queue = s->queue_sel % QACC_QUEUE_COUNT;
    QAccQueue *q = &s->queues[queue];
    uint32_t len = q->len;

    if (len > QACC_MAX_XFER) {
        len = QACC_MAX_XFER;
    }

    q->status = QACC_STATUS_BUSY;
    if (qacc_execute_xor(s, q, q->src, q->dst, len) < 0) {
        q->status = QACC_STATUS_DONE | QACC_STATUS_FAULT;
        if (cmd & QACC_CMD_IRQ) {
            qacc_raise_irq(s, queue);
        }
        return;
    }
    q->status = QACC_STATUS_DONE;

    if (cmd & QACC_CMD_IRQ) {
        qacc_raise_irq(s, queue);
    }
}

static void qacc_process_queue(QuardAmpAccelState *s, uint32_t queue)
{
    QAccQueue *q = &s->queues[queue % QACC_QUEUE_COUNT];
    bool completed = false;

    if (!q->sq_base || !q->cq_base || !q->sq_size || !q->cq_size) {
        return;
    }

    q->status = QACC_STATUS_BUSY;
    while (q->sq_head != q->sq_tail) {
        QAccDesc desc;
        QAccCqe cqe;
        uint64_t desc_addr = q->sq_base + (uint64_t)q->sq_head * QACC_DESC_SIZE;
        uint64_t cqe_addr = q->cq_base + (uint64_t)q->cq_tail * QACC_CQE_SIZE;
        uint64_t src;
        uint64_t dst;
        uint32_t len;
        uint16_t opcode;
        int32_t status = 0;

        pci_dma_read(&s->parent_obj, desc_addr, &desc, sizeof(desc));
        src = le64_to_cpu(desc.src);
        dst = le64_to_cpu(desc.dst);
        len = le32_to_cpu(desc.len);
        opcode = le16_to_cpu(desc.opcode);

        if (len > QACC_MAX_XFER || opcode != QACC_OP_XOR) {
            status = -1;
        } else if (qacc_execute_xor(s, q, src, dst, len) < 0) {
            status = -2;
        }

        cqe.job_id = desc.job_id;
        cqe.status = cpu_to_le32(status);
        cqe.len = cpu_to_le32(len);
        cqe.flags = cpu_to_le32(status ? QACC_STATUS_FAULT : QACC_STATUS_DONE);
        pci_dma_write(&s->parent_obj, cqe_addr, &cqe, sizeof(cqe));

        q->status = status ? QACC_STATUS_DONE | QACC_STATUS_FAULT
                           : QACC_STATUS_DONE;
        q->sq_head = (q->sq_head + 1) % q->sq_size;
        q->cq_tail = (q->cq_tail + 1) % q->cq_size;
        completed = true;
    }

    if (completed) {
        qacc_raise_irq(s, queue);
    }
}

static uint64_t qacc_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    QuardAmpAccelState *s = opaque;
    QAccQueue *q = qacc_selected_queue(s);

    if (size != 4) {
        return 0xffffffffU;
    }

    switch (addr) {
    case QACC_REG_MAGIC:
        return QACC_MAGIC;
    case QACC_REG_STATUS:
        return q->status;
    case QACC_REG_SRC_LO:
        return (uint32_t)q->src;
    case QACC_REG_SRC_HI:
        return (uint32_t)(q->src >> 32);
    case QACC_REG_DST_LO:
        return (uint32_t)q->dst;
    case QACC_REG_DST_HI:
        return (uint32_t)(q->dst >> 32);
    case QACC_REG_LEN:
        return q->len;
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
    case QACC_REG_QUEUE_SEL:
        return s->queue_sel;
    case QACC_REG_QUEUE_DOMAIN:
        return q->domain;
    case QACC_REG_QUEUE_VECTOR:
        return q->vector;
    case QACC_REG_QUEUE_COUNT:
        return QACC_QUEUE_COUNT;
    case QACC_REG_SQ_BASE_LO:
        return (uint32_t)q->sq_base;
    case QACC_REG_SQ_BASE_HI:
        return (uint32_t)(q->sq_base >> 32);
    case QACC_REG_SQ_SIZE:
        return q->sq_size;
    case QACC_REG_SQ_TAIL:
        return q->sq_tail;
    case QACC_REG_CQ_BASE_LO:
        return (uint32_t)q->cq_base;
    case QACC_REG_CQ_BASE_HI:
        return (uint32_t)(q->cq_base >> 32);
    case QACC_REG_CQ_SIZE:
        return q->cq_size;
    case QACC_REG_CQ_HEAD:
        return q->cq_head;
    case QACC_REG_CQ_TAIL:
        return q->cq_tail;
    default:
        return 0;
    }
}

static void qacc_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    QuardAmpAccelState *s = opaque;
    QAccQueue *q = qacc_selected_queue(s);

    if (size != 4) {
        return;
    }

    switch (addr) {
    case QACC_REG_STATUS:
        q->status &= ~(uint32_t)val;
        break;
    case QACC_REG_SRC_LO:
        q->src = (q->src & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case QACC_REG_SRC_HI:
        q->src = (q->src & 0xffffffffULL) | ((uint64_t)(uint32_t)val << 32);
        break;
    case QACC_REG_DST_LO:
        q->dst = (q->dst & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case QACC_REG_DST_HI:
        q->dst = (q->dst & 0xffffffffULL) | ((uint64_t)(uint32_t)val << 32);
        break;
    case QACC_REG_LEN:
        q->len = (uint32_t)val;
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
            q->status &= ~QACC_STATUS_FAULT;
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
    case QACC_REG_QUEUE_SEL:
        s->queue_sel = (uint32_t)val % QACC_QUEUE_COUNT;
        break;
    case QACC_REG_QUEUE_DOMAIN:
        q->domain = (uint32_t)val % QACC_QUEUE_COUNT;
        break;
    case QACC_REG_QUEUE_VECTOR:
        q->vector = (uint32_t)val % QACC_QUEUE_COUNT;
        break;
    case QACC_REG_SQ_BASE_LO:
        q->sq_base = (q->sq_base & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case QACC_REG_SQ_BASE_HI:
        q->sq_base = (q->sq_base & 0xffffffffULL) |
                     ((uint64_t)(uint32_t)val << 32);
        break;
    case QACC_REG_SQ_SIZE:
        q->sq_size = (uint32_t)val;
        q->sq_head = 0;
        q->sq_tail = 0;
        break;
    case QACC_REG_SQ_TAIL:
        if (q->sq_size) {
            q->sq_tail = (uint32_t)val % q->sq_size;
        }
        break;
    case QACC_REG_CQ_BASE_LO:
        q->cq_base = (q->cq_base & 0xffffffff00000000ULL) | (uint32_t)val;
        break;
    case QACC_REG_CQ_BASE_HI:
        q->cq_base = (q->cq_base & 0xffffffffULL) |
                     ((uint64_t)(uint32_t)val << 32);
        break;
    case QACC_REG_CQ_SIZE:
        q->cq_size = (uint32_t)val;
        q->cq_head = 0;
        q->cq_tail = 0;
        break;
    case QACC_REG_CQ_HEAD:
        if (q->cq_size) {
            q->cq_head = (uint32_t)val % q->cq_size;
        }
        break;
    case QACC_REG_DOORBELL:
        qacc_process_queue(s, (uint32_t)val);
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
    s->queues[0].domain = 0;
    s->queues[0].vector = 0;
    s->queues[1].domain = 1;
    s->queues[1].vector = 1;

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    memory_region_init_io(&s->bar0, OBJECT(s), &qacc_bar0_ops, s,
                          "quardamp-accel-bar0", 64 * KiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    if (msix_init_exclusive_bar(pdev, QACC_QUEUE_COUNT, QACC_MSIX_BAR, errp)) {
        return;
    }
    for (uint32_t i = 0; i < QACC_QUEUE_COUNT; i++) {
        msix_vector_use(pdev, i);
    }
}

static void qacc_exit(PCIDevice *pdev)
{
    msix_uninit_exclusive_bar(pdev);
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
