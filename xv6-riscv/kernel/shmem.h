#ifndef XV6_SHMEM_H
#define XV6_SHMEM_H

#include "memlayout.h"

/*
 * AMP shared-memory ABI used by xv6 and FreeRTOS.
 *
 * Version 2 replaces the old fixed shmem_msg slots with two virtio-style
 * split rings.  Each descriptor points at one 512-byte buffer whose first bytes
 * are a Linux-compatible rpmsg_hdr.  The mailbox remains only a vring kick.
 */
#define RPMSG_NAME_SIZE 32
#define RPMSG_NS_ADDR   53
#define RPMSG_NS_CREATE 0
#define RPMSG_NS_DESTROY 1
#define VIRTIO_ID_RPMSG 7
#define VIRTIO_RPMSG_F_NS 0

#define SHMEM_EP_XV6_TEST    0x00000100
#define SHMEM_EP_RTOS_ECHO   0x00010100
#define SHMEM_EP_RTOS_UPPER  0x00010200

#define SHMEM_CMD_TEST       0x00000200

#define RPMSG_HDR_SIZE     16
#define RPMSG_APP_HDR_SIZE 12
#define SHMSG_SLOT_NUM      SHMEM_RING_SIZE
#define SHMSG_PAYLOAD_SIZE  (SHMEM_RPMSG_BUF_SIZE - RPMSG_HDR_SIZE - RPMSG_APP_HDR_SIZE)

#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2
#define VRING_DESC_F_INDIRECT 4

struct shmem_ctrl {
  volatile uint32 magic;
  volatile uint32 version;
  volatile uint32 features;
  volatile uint32 reserved;
};

struct vring_desc {
  uint64 addr;
  uint32 len;
  uint16 flags;
  uint16 next;
} __attribute__((packed));

struct vring_avail {
  uint16 flags;
  volatile uint16 idx;
  uint16 ring[SHMSG_SLOT_NUM];
} __attribute__((packed));

struct vring_used_elem {
  uint32 id;
  uint32 len;
} __attribute__((packed));

struct vring_used {
  uint16 flags;
  volatile uint16 idx;
  struct vring_used_elem ring[SHMSG_SLOT_NUM];
} __attribute__((packed));

struct rpmsg_hdr {
  uint32 src;
  uint32 dst;
  uint32 reserved;
  uint16 len;
  uint16 flags;
  uint8 data[];
} __attribute__((packed));

struct rpmsg_ns_msg {
  char name[RPMSG_NAME_SIZE];
  uint32 addr;
  uint32 flags;
} __attribute__((packed));

/*
 * Existing xv6 tests still pass cmd/cookie through the syscall ABI.  Those
 * fields are now application payload, not rpmsg transport header fields.
 */
struct rpmsg_app_hdr {
  uint32 cmd;
  uint32 cookie;
  uint32 flags;
} __attribute__((packed));

struct icc_msg {
  uint32 src_ep;
  uint32 dst_ep;
  uint32 cmd;
  uint32 len;
  uint32 cookie;
  uint32 flags;
  char payload[SHMSG_PAYLOAD_SIZE];
};

#define SHMEM_CTRL_BASE \
  ((volatile struct shmem_ctrl *)(SHMEM_ADDR + SHMEM_CTRL_OFFSET))

#define SHMEM_TO_RTOS_DESC \
  ((volatile struct vring_desc *)(SHMEM_ADDR + SHMEM_VRING_TO_RTOS_OFFSET))
#define SHMEM_TO_RTOS_AVAIL \
  ((volatile struct vring_avail *)(SHMEM_ADDR + SHMEM_VRING_TO_RTOS_OFFSET + 0x400))
#define SHMEM_TO_RTOS_USED \
  ((volatile struct vring_used *)(SHMEM_ADDR + SHMEM_VRING_TO_RTOS_OFFSET + 0x1000))
#define SHMEM_TO_RTOS_BUF_BASE \
  ((volatile uint8 *)(SHMEM_ADDR + SHMEM_BUF_TO_RTOS_OFFSET))

#define SHMEM_TO_XV6_DESC \
  ((volatile struct vring_desc *)(SHMEM_ADDR + SHMEM_VRING_TO_XV6_OFFSET))
#define SHMEM_TO_XV6_AVAIL \
  ((volatile struct vring_avail *)(SHMEM_ADDR + SHMEM_VRING_TO_XV6_OFFSET + 0x400))
#define SHMEM_TO_XV6_USED \
  ((volatile struct vring_used *)(SHMEM_ADDR + SHMEM_VRING_TO_XV6_OFFSET + 0x1000))
#define SHMEM_TO_XV6_BUF_BASE \
  ((volatile uint8 *)(SHMEM_ADDR + SHMEM_BUF_TO_XV6_OFFSET))

#endif
