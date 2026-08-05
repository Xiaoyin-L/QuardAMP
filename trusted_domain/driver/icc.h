#ifndef ICC_H
#define ICC_H

#include <stdint.h>
#include "shmem.h"

/*
 * ICC (inter-core communication) turns the raw shared-memory rings from
 * stage 3 into a small message API:
 *
 *   mailbox ISR -> drain to_rtos ring -> queue by value
 *       -> vIccDispatchTask -> endpoint handler
 *
 * Sending uses loan/send/release.  loan() reserves the current to_xv6 tail
 * slot while holding a mutex, send() publishes it and rings xv6, and release()
 * aborts an unused loan.
 */

#define ICC_MAX_HANDLERS 8
#define ICC_DISPATCH_QUEUE_DEPTH 8

typedef void (*icc_handler_t)(struct shmem_msg *msg);

void icc_init(void);
int icc_register_handler(uint32_t ep, icc_handler_t handler);
struct shmem_msg *icc_message_loan(uint32_t dst_ep);
int icc_message_send(struct shmem_msg *msg);
void icc_message_release(struct shmem_msg *msg);
void icc_isr_drain_to_rtos(void);
void vIccDispatchTask(void *p_arg);
void vIccTestTask(void *p_arg);
void icc_echo_handler(struct shmem_msg *msg);

#endif /* ICC_H */
