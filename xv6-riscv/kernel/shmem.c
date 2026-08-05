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
 * Stage 3 owned both ring send and receive here.  Stage 5 moves active ring
 * operations into icc.c:
 *   - icc_send() serializes writers and publishes to the to_rtos ring.
 *   - icc_notify_recv() drains the to_xv6 ring and wakes endpoint sleepers.
 *
 * This file stays as the shared-memory reset point plus the old shmemsend()
 * syscall backend, so earlier tests keep working on top of the new ICC layer.
 */
static const char shmem_test_payload[] = "xv6->rtos phase5 hello";

void
shmem_init(void)
{
  if(sizeof(struct shmem_msg) != SHMEM_MSG_SIZE)
    panic("shmem_msg size");

  memset((void*)SHMEM_CTRL_BASE, 0, SHMEM_CTRL_SIZE);
  memset((void*)SHMEM_TO_RTOS_BASE, 0, SHMEM_RING_BYTES);
  memset((void*)SHMEM_TO_XV6_BASE, 0, SHMEM_RING_BYTES);

  SHMEM_CTRL_BASE->version = SHMEM_VERSION;
  __sync_synchronize();
  SHMEM_CTRL_BASE->magic = SHMEM_MAGIC;

  printf("shmem: addr=%p size=%d ctrl=%p to_rtos=%p to_xv6=%p\n",
         (void*)SHMEM_ADDR, SHMEM_SIZE, (void*)SHMEM_CTRL_BASE,
         (void*)SHMEM_TO_RTOS_BASE, (void*)SHMEM_TO_XV6_BASE);
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
