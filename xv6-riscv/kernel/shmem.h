#ifndef XV6_SHMEM_H
#define XV6_SHMEM_H

#include "memlayout.h"

/*
 * AMP shared-memory ABI shared by xv6 kernel code and small user tests.
 *
 * The physical layout is reserved by OpenSBI domains and mapped into xv6 by
 * vm.c.  The control page is followed by two fixed-size rings:
 *   to_rtos: xv6 writes, FreeRTOS consumes.
 *   to_xv6 : FreeRTOS writes, xv6 consumes.
 *
 * Keep this header intentionally small: it describes the wire format only.
 * Policy such as endpoint sleep/wakeup belongs in kernel/icc.c.
 */
#define SHMEM_EP_XV6_TEST    0x00000100
#define SHMEM_EP_RTOS_ECHO   0x00010100
#define SHMEM_EP_RTOS_UPPER  0x00010200
#define SHMEM_CMD_TEST     0x00000200

#define SHMSG_SLOT_NUM      SHMEM_RING_SIZE
#define SHMSG_SLOT_SIZE     SHMEM_MSG_SIZE
#define SHMSG_PAYLOAD_SIZE  40

struct shmem_ctrl {
  volatile uint32 magic;
  volatile uint32 version;
  volatile uint32 to_rtos_head;
  volatile uint32 to_rtos_tail;
  volatile uint32 to_xv6_head;
  volatile uint32 to_xv6_tail;
};

struct shmem_msg {
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
#define SHMEM_TO_RTOS_BASE \
  ((volatile struct shmem_msg *)(SHMEM_ADDR + SHMEM_TO_RTOS_OFFSET))
#define SHMEM_TO_XV6_BASE \
  ((volatile struct shmem_msg *)(SHMEM_ADDR + SHMEM_TO_XV6_OFFSET))

#endif
