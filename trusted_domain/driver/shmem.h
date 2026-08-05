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
#define SHMEM_CMD_TEST     0x00000200U

struct shmem_ctrl {
    volatile uint32_t magic;
    volatile uint32_t version;
    volatile uint32_t to_rtos_head;
    volatile uint32_t to_rtos_tail;
    volatile uint32_t to_xv6_head;
    volatile uint32_t to_xv6_tail;
};

struct shmem_msg {
    uint32_t src_ep;
    uint32_t dst_ep;
    uint32_t cmd;
    uint32_t len;
    uint32_t cookie;
    uint32_t flags;
    char payload[40];
};

#define SHMEM_CTRL_BASE \
    ((volatile struct shmem_ctrl *)(SHMEM_ADDR + SHMEM_CTRL_OFFSET))
#define SHMEM_TO_RTOS_BASE \
    ((volatile struct shmem_msg *)(SHMEM_ADDR + SHMEM_TO_RTOS_OFFSET))
#define SHMEM_TO_XV6_BASE \
    ((volatile struct shmem_msg *)(SHMEM_ADDR + SHMEM_TO_XV6_OFFSET))

/*
 * Stage 4 routes mailbox doorbells through ICC:
 *   IRQ -> icc_isr_drain_to_rtos() -> queue -> vIccDispatchTask().
 * The old stage-3 shmem_handle_to_rtos_doorbell() direct ISR handler is
 * intentionally removed.
 */

#endif /* SHMEM_H */
