#ifndef ICC_H
#define ICC_H

#include <stdint.h>
#include "shmem.h"

/*
 * ICC now transports Linux-style rpmsg buffers over the QuardAMP mailbox.
 * FreeRTOS handlers receive parsed application messages, while outbound
 * replies use loan/send/release on the underlying rpmsg buffer pool.
 */

#define ICC_MAX_HANDLERS 8
#define ICC_DISPATCH_QUEUE_DEPTH 8

typedef void (*icc_handler_t)(struct icc_msg *msg);

void icc_init(void);
int icc_register_handler(uint32_t ep, icc_handler_t handler);
struct rpmsg_hdr *icc_message_loan(uint32_t dst_ep);
int icc_message_send(struct rpmsg_hdr *msg);
void icc_message_release(struct rpmsg_hdr *msg);
void icc_prepare_app_message(struct rpmsg_hdr *hdr, uint32_t src_ep,
                             uint32_t dst_ep, uint32_t cmd, uint32_t cookie,
                             uint32_t flags, const char *payload,
                             uint32_t len);
void icc_isr_drain_to_rtos(void);
void vIccDispatchTask(void *p_arg);
void vIccTestTask(void *p_arg);
void icc_echo_handler(struct icc_msg *msg);

/*
 * RPC demo service.  The dispatch task calls this handler for
 * SHMEM_EP_RTOS_UPPER; it uppercases the request payload, preserves the cookie,
 * and sends the transformed payload back to the xv6 source endpoint.
 */
void icc_upper_handler(struct icc_msg *msg);

#endif /* ICC_H */
