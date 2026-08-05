#ifndef SHMEM_H
#define SHMEM_H

#include <stdint.h>

/*
 * AMP shared memory layout. Keep these constants aligned with
 * xv6-riscv/kernel/memlayout.h and dts/quard_star_sbi.dts.
 */
#define SHMEM_ADDR 0xb2000000UL
#define SHMEM_SIZE (1024UL * 1024UL)
#define SHMEM_CTRL_OFFSET    0x0000UL
#define SHMEM_TO_RTOS_OFFSET 0x1000UL
#define SHMEM_TO_XV6_OFFSET  0x2000UL
#define SHMEM_RING_SIZE 64U
#define SHMEM_MSG_SIZE 64U
#define SHMEM_MAGIC 0x51414d50U
#define SHMEM_VERSION 1U
#define SHMEM_DOORBELL_CH0 0U

#define SHMEM_EP_XV6_TEST  0x00000100U
#define SHMEM_EP_RTOS_ECHO 0x00010100U
#define SHMEM_CMD_TEST     0x00000100U

void shmem_handle_to_rtos_doorbell(void);

#endif /* SHMEM_H */
