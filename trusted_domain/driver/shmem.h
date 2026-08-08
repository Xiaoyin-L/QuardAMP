#ifndef SHMEM_H
#define SHMEM_H

#include <stdint.h>

/*
 * AMP shared memory layout. Keep these constants aligned with
 * xv6-riscv/kernel/memlayout.h and dts/quard_star_sbi.dts.
 *
 * Version 2 uses virtio split rings carrying Linux rpmsg_hdr buffers.  The
 * mailbox device is only the notification path for updated vrings.
 */
#define SHMEM_ADDR 0xb2000000UL
#define SHMEM_SIZE (1024UL * 1024UL)
#define SHMEM_CTRL_OFFSET    0x0000UL
#define SHMEM_VRING_TO_RTOS_OFFSET 0x1000UL
#define SHMEM_VRING_TO_XV6_OFFSET  0x3000UL
#define SHMEM_BUF_TO_RTOS_OFFSET   0x5000UL
#define SHMEM_BUF_TO_XV6_OFFSET    0xd000UL
#define SHMEM_CTRL_SIZE 0x1000UL
#define SHMEM_RING_SIZE 64U
#define SHMEM_VRING_BYTES 0x2000UL
#define SHMEM_RPMSG_BUF_SIZE 512U
#define SHMEM_RPMSG_BUF_BYTES (SHMEM_RING_SIZE * SHMEM_RPMSG_BUF_SIZE)
#define SHMEM_MAGIC 0x51414d50U
#define SHMEM_VERSION 2U
#define SHMEM_DOORBELL_VRING_TO_RTOS (1U << 0)
#define SHMEM_DOORBELL_VRING_TO_XV6  (1U << 1)
#define SHMEM_DOORBELL_CH0 SHMEM_DOORBELL_VRING_TO_RTOS

#define RPMSG_NAME_SIZE 32U
#define RPMSG_NS_ADDR   53U
#define RPMSG_NS_CREATE 0U
#define RPMSG_NS_DESTROY 1U
#define VIRTIO_ID_RPMSG 7U
#define VIRTIO_RPMSG_F_NS 0U

#define SHMEM_EP_XV6_TEST    0x00000100U
#define SHMEM_EP_RTOS_ECHO   0x00010100U
#define SHMEM_EP_RTOS_UPPER  0x00010200U
#define SHMEM_EP_RTOS_BENCH  0x00010300U

#define SHMEM_CMD_TEST       0x00000200U
#define SHMEM_CMD_BENCH      0x00000500U

#define RPMSG_HDR_SIZE     16U
#define RPMSG_APP_HDR_SIZE 12U
#define SHMSG_SLOT_NUM      SHMEM_RING_SIZE
#define SHMSG_PAYLOAD_SIZE  (SHMEM_RPMSG_BUF_SIZE - RPMSG_HDR_SIZE - RPMSG_APP_HDR_SIZE)

#define VRING_DESC_F_NEXT     1U
#define VRING_DESC_F_WRITE    2U
#define VRING_DESC_F_INDIRECT 4U

struct shmem_ctrl {
    volatile uint32_t magic;
    volatile uint32_t version;
    volatile uint32_t features;
    volatile uint32_t reserved;
};

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    volatile uint16_t idx;
    uint16_t ring[SHMSG_SLOT_NUM];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    volatile uint16_t idx;
    struct vring_used_elem ring[SHMSG_SLOT_NUM];
} __attribute__((packed));

struct rpmsg_hdr {
    uint32_t src;
    uint32_t dst;
    uint32_t reserved;
    uint16_t len;
    uint16_t flags;
    uint8_t data[];
} __attribute__((packed));

struct rpmsg_ns_msg {
    char name[RPMSG_NAME_SIZE];
    uint32_t addr;
    uint32_t flags;
} __attribute__((packed));

struct rpmsg_app_hdr {
    uint32_t cmd;
    uint32_t cookie;
    uint32_t flags;
} __attribute__((packed));

struct icc_msg {
    uint32_t src_ep;
    uint32_t dst_ep;
    uint32_t cmd;
    uint32_t len;
    uint32_t cookie;
    uint32_t flags;
    char payload[SHMSG_PAYLOAD_SIZE];
};

#define SHMEM_CTRL_BASE \
    ((volatile struct shmem_ctrl *)(SHMEM_ADDR + SHMEM_CTRL_OFFSET))

#define SHMEM_TO_RTOS_DESC \
    ((volatile struct vring_desc *)(SHMEM_ADDR + SHMEM_VRING_TO_RTOS_OFFSET))
#define SHMEM_TO_RTOS_AVAIL \
    ((volatile struct vring_avail *)(SHMEM_ADDR + SHMEM_VRING_TO_RTOS_OFFSET + 0x400UL))
#define SHMEM_TO_RTOS_USED \
    ((volatile struct vring_used *)(SHMEM_ADDR + SHMEM_VRING_TO_RTOS_OFFSET + 0x1000UL))
#define SHMEM_TO_RTOS_BUF_BASE \
    ((volatile uint8_t *)(SHMEM_ADDR + SHMEM_BUF_TO_RTOS_OFFSET))

#define SHMEM_TO_XV6_DESC \
    ((volatile struct vring_desc *)(SHMEM_ADDR + SHMEM_VRING_TO_XV6_OFFSET))
#define SHMEM_TO_XV6_AVAIL \
    ((volatile struct vring_avail *)(SHMEM_ADDR + SHMEM_VRING_TO_XV6_OFFSET + 0x400UL))
#define SHMEM_TO_XV6_USED \
    ((volatile struct vring_used *)(SHMEM_ADDR + SHMEM_VRING_TO_XV6_OFFSET + 0x1000UL))
#define SHMEM_TO_XV6_BUF_BASE \
    ((volatile uint8_t *)(SHMEM_ADDR + SHMEM_BUF_TO_XV6_OFFSET))

#endif /* SHMEM_H */
