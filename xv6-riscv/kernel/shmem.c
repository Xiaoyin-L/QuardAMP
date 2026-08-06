#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "shmem.h"

/*
 * shmem.c -- xv6-side shared-memory initialization and compatibility wrapper.
 *
 * Stage 3 owned both custom rings here.  The rpmsg transport now keeps active
 * split-ring operations in icc.c, while this file resets and describes the
 * shared vring/buffer layout.
 *
 * This file stays as the shared-memory reset point plus the old shmemsend()
 * syscall backend, so earlier tests keep working on top of the new ICC layer.
 */
static const char shmem_test_payload[] = "xv6->rtos phase5 hello";

void
shmem_init(void)
{
  memset((void*)SHMEM_CTRL_BASE, 0, SHMEM_CTRL_SIZE);
  memset((void*)SHMEM_TO_RTOS_DESC, 0, SHMEM_VRING_BYTES);
  memset((void*)SHMEM_TO_XV6_DESC, 0, SHMEM_VRING_BYTES);
  memset((void*)SHMEM_TO_RTOS_BUF_BASE, 0, SHMEM_RPMSG_BUF_BYTES);
  memset((void*)SHMEM_TO_XV6_BUF_BASE, 0, SHMEM_RPMSG_BUF_BYTES);

  SHMEM_CTRL_BASE->features = (1U << VIRTIO_RPMSG_F_NS);
  SHMEM_CTRL_BASE->version = SHMEM_VERSION;
  __sync_synchronize();
  SHMEM_CTRL_BASE->magic = SHMEM_MAGIC;

  printf("shmem: addr=%p size=%d ctrl=%p vring_tx=%p vring_rx=%p buf=%d\n",
         (void*)SHMEM_ADDR, SHMEM_SIZE, (void*)SHMEM_CTRL_BASE,
         (void*)SHMEM_TO_RTOS_DESC, (void*)SHMEM_TO_XV6_DESC,
         SHMEM_RPMSG_BUF_SIZE);
}

int
shmem_send_to_rtos(uint32 cookie)
{
  uint32 len;

  len = strlen(shmem_test_payload);
  if(len > SHMSG_PAYLOAD_SIZE)
    len = SHMSG_PAYLOAD_SIZE;

  return icc_send(SHMEM_EP_RTOS_ECHO, SHMEM_CMD_TEST,
                  shmem_test_payload, len, cookie);
}
