#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>
#include <stdint.h>
#include "debug_log.h"
#include "mailbox.h"
#include "icc.h"

struct icc_handler_entry {
    uint32_t ep;
    icc_handler_t handler;
};

static QueueHandle_t xIccDispatchQueue;
static SemaphoreHandle_t xIccSendMutex;
static struct icc_handler_entry icc_handlers[ICC_MAX_HANDLERS];
static uint16_t icc_to_rtos_last_avail;

#ifndef ICC_VERBOSE
#define ICC_VERBOSE 0
#endif

#if ICC_VERBOSE
#define icc_trace(...) debug_log(__VA_ARGS__)
#else
#define icc_trace(...) do { } while (0)
#endif

static inline void icc_fence(void)
{
    __asm__ volatile ("fence rw, rw" ::: "memory");
}

static inline uint64_t icc_rdtime(void)
{
    uint64_t value;

    __asm__ volatile ("rdtime %0" : "=r" (value));
    return value;
}

static unsigned int payload_len(const char *s)
{
    unsigned int len = 0;

    while (s[len] != '\0') {
        len++;
    }
    return len;
}

static void copy_from_volatile(void *dst, volatile void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    volatile uint8_t *s = (volatile uint8_t *)src;

    for (uint32_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void clear_volatile(volatile void *ptr, uint32_t n)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;

    for (uint32_t i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static void payload_to_string(char *dst, unsigned int dst_size,
                              const struct icc_msg *msg)
{
    unsigned int len = msg->len;

    if (dst_size == 0) {
        return;
    }
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    if (len > sizeof(msg->payload)) {
        len = sizeof(msg->payload);
    }

    for (unsigned int i = 0; i < len; i++) {
        dst[i] = msg->payload[i];
    }
    dst[len] = '\0';
}

static int rpmsg_decode_app(uint8_t *wire, uint32_t wire_len,
                            struct icc_msg *out)
{
    struct rpmsg_hdr *hdr = (struct rpmsg_hdr *)wire;
    struct rpmsg_app_hdr *app;
    uint32_t payload_len;

    if (wire_len < RPMSG_HDR_SIZE || hdr->len < RPMSG_APP_HDR_SIZE) {
        return -1;
    }
    if (RPMSG_HDR_SIZE + hdr->len > wire_len) {
        return -1;
    }

    app = (struct rpmsg_app_hdr *)hdr->data;
    payload_len = hdr->len - RPMSG_APP_HDR_SIZE;
    if (payload_len > SHMSG_PAYLOAD_SIZE) {
        payload_len = SHMSG_PAYLOAD_SIZE;
    }

    out->src_ep = hdr->src;
    out->dst_ep = hdr->dst;
    out->cmd = app->cmd;
    out->cookie = app->cookie;
    out->flags = app->flags;
    out->len = payload_len;
    for (uint32_t i = 0; i < payload_len; i++) {
        out->payload[i] = (char)hdr->data[RPMSG_APP_HDR_SIZE + i];
    }
    return 0;
}

void icc_prepare_app_message(struct rpmsg_hdr *hdr, uint32_t src_ep,
                             uint32_t dst_ep, uint32_t cmd, uint32_t cookie,
                             uint32_t flags, const char *payload,
                             uint32_t len)
{
    volatile struct rpmsg_hdr *vhdr = (volatile struct rpmsg_hdr *)hdr;
    volatile struct rpmsg_app_hdr *app;
    volatile uint8_t *data;

    if (len > SHMSG_PAYLOAD_SIZE) {
        len = SHMSG_PAYLOAD_SIZE;
    }

    clear_volatile(vhdr, SHMEM_RPMSG_BUF_SIZE);
    vhdr->src = src_ep;
    vhdr->dst = dst_ep;
    vhdr->reserved = 0;
    vhdr->flags = 0;
    vhdr->len = RPMSG_APP_HDR_SIZE + len;

    app = (volatile struct rpmsg_app_hdr *)vhdr->data;
    app->cmd = cmd;
    app->cookie = cookie;
    app->flags = flags;

    data = vhdr->data + RPMSG_APP_HDR_SIZE;
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)payload[i];
    }
}

static void icc_prepare_ns_message(struct rpmsg_hdr *hdr, const char *name,
                                   uint32_t addr)
{
    volatile struct rpmsg_hdr *vhdr = (volatile struct rpmsg_hdr *)hdr;
    volatile struct rpmsg_ns_msg *ns;
    uint32_t len = payload_len(name);

    if (len >= RPMSG_NAME_SIZE) {
        len = RPMSG_NAME_SIZE - 1;
    }

    clear_volatile(vhdr, SHMEM_RPMSG_BUF_SIZE);
    vhdr->src = addr;
    vhdr->dst = RPMSG_NS_ADDR;
    vhdr->reserved = 0;
    vhdr->flags = 0;
    vhdr->len = sizeof(struct rpmsg_ns_msg);

    ns = (volatile struct rpmsg_ns_msg *)vhdr->data;
    for (uint32_t i = 0; i < len; i++) {
        ns->name[i] = name[i];
    }
    ns->name[len] = '\0';
    ns->addr = addr;
    ns->flags = RPMSG_NS_CREATE;
}

void icc_init(void)
{
    xIccDispatchQueue = xQueueCreate(ICC_DISPATCH_QUEUE_DEPTH,
                                     sizeof(struct icc_msg));
    xIccSendMutex = xSemaphoreCreateMutex();
    icc_to_rtos_last_avail = 0;

    for (int i = 0; i < ICC_MAX_HANDLERS; i++) {
        icc_handlers[i].ep = 0;
        icc_handlers[i].handler = NULL;
    }

    if (xIccDispatchQueue == NULL || xIccSendMutex == NULL) {
        debug_log("rpmsg init failed: queue=%p mutex=%p\n",
                  (unsigned long)xIccDispatchQueue,
                  (unsigned long)xIccSendMutex);
        return;
    }

    debug_log("rpmsg init: queue depth=%d handlers=%d buf=%d payload=%d\n",
              ICC_DISPATCH_QUEUE_DEPTH, ICC_MAX_HANDLERS,
              SHMEM_RPMSG_BUF_SIZE, SHMSG_PAYLOAD_SIZE);
}

int icc_register_handler(uint32_t ep, icc_handler_t handler)
{
    if (handler == NULL) {
        return -1;
    }

    for (int i = 0; i < ICC_MAX_HANDLERS; i++) {
        if (icc_handlers[i].handler != NULL && icc_handlers[i].ep == ep) {
            icc_handlers[i].handler = handler;
            return 0;
        }
    }

    for (int i = 0; i < ICC_MAX_HANDLERS; i++) {
        if (icc_handlers[i].handler == NULL) {
            icc_handlers[i].ep = ep;
            icc_handlers[i].handler = handler;
            icc_trace("rpmsg register: ep=%x slot=%d\n",
                      (unsigned long)ep, i);
            return 0;
        }
    }

    debug_log("rpmsg register failed: table full ep=%x\n", (unsigned long)ep);
    return -1;
}

void icc_isr_drain_to_rtos(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (xIccDispatchQueue == NULL) {
        return;
    }
    if (SHMEM_CTRL_BASE->magic != SHMEM_MAGIC ||
        SHMEM_CTRL_BASE->version != SHMEM_VERSION) {
        return;
    }

    for (;;) {
        uint16_t avail_idx = SHMEM_TO_RTOS_AVAIL->idx;
        uint16_t used_idx;
        uint16_t desc_id;
        uint32_t wire_len;
        uint8_t wire[SHMEM_RPMSG_BUF_SIZE];
        struct icc_msg msg;
        volatile struct vring_desc *desc;
        volatile uint8_t *buf;

        if (icc_to_rtos_last_avail == avail_idx) {
            break;
        }

        desc_id = SHMEM_TO_RTOS_AVAIL->ring[
            icc_to_rtos_last_avail % SHMSG_SLOT_NUM];
        if (desc_id >= SHMSG_SLOT_NUM) {
            icc_to_rtos_last_avail++;
            continue;
        }

        desc = &SHMEM_TO_RTOS_DESC[desc_id];
        wire_len = desc->len;
        if (wire_len > SHMEM_RPMSG_BUF_SIZE) {
            wire_len = SHMEM_RPMSG_BUF_SIZE;
        }
        buf = (volatile uint8_t *)(uintptr_t)desc->addr;

        icc_fence();
        copy_from_volatile(wire, buf, wire_len);

        used_idx = SHMEM_TO_RTOS_USED->idx;
        SHMEM_TO_RTOS_USED->ring[used_idx % SHMSG_SLOT_NUM].id = desc_id;
        SHMEM_TO_RTOS_USED->ring[used_idx % SHMSG_SLOT_NUM].len = desc->len;
        icc_fence();
        SHMEM_TO_RTOS_USED->idx = used_idx + 1;
        icc_fence();
        icc_to_rtos_last_avail++;

        if (rpmsg_decode_app(wire, wire_len, &msg) < 0) {
            debug_log("rpmsg isr: bad buffer len=%d\n", (int)wire_len);
            continue;
        }

        if (xQueueSendFromISR(xIccDispatchQueue, &msg,
                              &xHigherPriorityTaskWoken) != pdTRUE) {
            debug_log("rpmsg isr: dispatch queue full, drop cookie=%x\n",
                      (unsigned long)msg.cookie);
        }
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

struct rpmsg_hdr *icc_message_loan(uint32_t dst_ep)
{
    uint16_t avail_idx;
    uint16_t used_idx;
    uint16_t desc_id;
    volatile uint8_t *buf;

    if (xIccSendMutex == NULL) {
        return NULL;
    }
    if (SHMEM_CTRL_BASE->magic != SHMEM_MAGIC ||
        SHMEM_CTRL_BASE->version != SHMEM_VERSION) {
        return NULL;
    }
    if (xSemaphoreTake(xIccSendMutex, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }

    avail_idx = SHMEM_TO_XV6_AVAIL->idx;
    used_idx = SHMEM_TO_XV6_USED->idx;
    if ((uint16_t)(avail_idx - used_idx) >= SHMSG_SLOT_NUM) {
        debug_log("rpmsg loan failed: to_xv6 full avail=%d used=%d\n",
                  (int)avail_idx, (int)used_idx);
        xSemaphoreGive(xIccSendMutex);
        return NULL;
    }

    desc_id = avail_idx % SHMSG_SLOT_NUM;
    buf = SHMEM_TO_XV6_BUF_BASE + desc_id * SHMEM_RPMSG_BUF_SIZE;
    clear_volatile(buf, SHMEM_RPMSG_BUF_SIZE);

    ((volatile struct rpmsg_hdr *)buf)->src = SHMEM_EP_RTOS_ECHO;
    ((volatile struct rpmsg_hdr *)buf)->dst = dst_ep;
    ((volatile struct rpmsg_hdr *)buf)->reserved = 0;
    ((volatile struct rpmsg_hdr *)buf)->flags = 0;
    ((volatile struct rpmsg_hdr *)buf)->len = 0;

    return (struct rpmsg_hdr *)buf;
}

int icc_message_send(struct rpmsg_hdr *msg)
{
    uint16_t avail_idx = SHMEM_TO_XV6_AVAIL->idx;
    uint16_t desc_id = avail_idx % SHMSG_SLOT_NUM;
    volatile uint8_t *expected =
        SHMEM_TO_XV6_BUF_BASE + desc_id * SHMEM_RPMSG_BUF_SIZE;
    uint32_t wire_len;

    if (msg == NULL) {
        return -1;
    }
    if ((volatile uint8_t *)msg != expected) {
        debug_log("rpmsg send failed: msg=%p expected=%p\n",
                  (unsigned long)msg, (unsigned long)expected);
        xSemaphoreGive(xIccSendMutex);
        return -1;
    }

    wire_len = RPMSG_HDR_SIZE + msg->len;
    if (wire_len > SHMEM_RPMSG_BUF_SIZE) {
        xSemaphoreGive(xIccSendMutex);
        return -1;
    }

    SHMEM_TO_XV6_DESC[desc_id].addr = (uint64_t)(uintptr_t)msg;
    SHMEM_TO_XV6_DESC[desc_id].len = wire_len;
    SHMEM_TO_XV6_DESC[desc_id].flags = 0;
    SHMEM_TO_XV6_DESC[desc_id].next = 0;
    SHMEM_TO_XV6_AVAIL->ring[desc_id] = desc_id;

    icc_fence();
    SHMEM_TO_XV6_AVAIL->idx = avail_idx + 1;
    icc_fence();
    mailbox_ring_to_xv6(SHMEM_DOORBELL_VRING_TO_XV6);
    xSemaphoreGive(xIccSendMutex);
    return 0;
}

void icc_message_release(struct rpmsg_hdr *msg)
{
    (void)msg;

    if (xIccSendMutex != NULL) {
        xSemaphoreGive(xIccSendMutex);
    }
}

static void icc_announce_service(const char *name, uint32_t addr)
{
    struct rpmsg_hdr *msg = icc_message_loan(RPMSG_NS_ADDR);

    if (msg == NULL) {
        debug_log("rpmsg ns: loan failed for %s\n", name);
        return;
    }

    icc_prepare_ns_message(msg, name, addr);
    (void)icc_message_send(msg);
}

void vIccDispatchTask(void *p_arg)
{
    struct icc_msg msg;

    (void)p_arg;

    for (;;) {
        if (xQueueReceive(xIccDispatchQueue, &msg, portMAX_DELAY) == pdTRUE) {
            int found = 0;

            for (int i = 0; i < ICC_MAX_HANDLERS; i++) {
                if (icc_handlers[i].handler != NULL &&
                    icc_handlers[i].ep == msg.dst_ep) {
                    icc_handlers[i].handler(&msg);
                    found = 1;
                    break;
                }
            }

            if (!found) {
                debug_log("rpmsg dispatch: no handler for dst=%x cookie=%x\n",
                          (unsigned long)msg.dst_ep,
                          (unsigned long)msg.cookie);
            }
        }
    }
}

void icc_echo_handler(struct icc_msg *msg)
{
    struct rpmsg_hdr *reply;
    char payload[sizeof(msg->payload) + 1];

    payload_to_string(payload, sizeof(payload), msg);
    icc_trace("rpmsg echo: rx src=%x dst=%x cmd=%x cookie=%x payload=%s\n",
              (unsigned long)msg->src_ep, (unsigned long)msg->dst_ep,
              (unsigned long)msg->cmd, (unsigned long)msg->cookie, payload);

    reply = icc_message_loan(msg->src_ep);
    if (reply == NULL) {
        debug_log("rpmsg echo: loan failed\n");
        return;
    }

    icc_prepare_app_message(reply, SHMEM_EP_RTOS_ECHO, msg->src_ep,
                            msg->cmd, msg->cookie, 0,
                            "rtos->xv6 rpmsg ack",
                            payload_len("rtos->xv6 rpmsg ack"));
    (void)icc_message_send(reply);
}

void icc_upper_handler(struct icc_msg *msg)
{
    struct rpmsg_hdr *reply;
    volatile struct rpmsg_app_hdr *app;
    volatile uint8_t *data;
    char payload[sizeof(msg->payload) + 1];
    uint32_t len = msg->len;

    payload_to_string(payload, sizeof(payload), msg);
    icc_trace("rpmsg upper: rx src=%x dst=%x cmd=%x cookie=%x payload=%s\n",
              (unsigned long)msg->src_ep, (unsigned long)msg->dst_ep,
              (unsigned long)msg->cmd, (unsigned long)msg->cookie, payload);

    reply = icc_message_loan(msg->src_ep);
    if (reply == NULL) {
        debug_log("rpmsg upper: loan failed\n");
        return;
    }

    if (len > SHMSG_PAYLOAD_SIZE) {
        len = SHMSG_PAYLOAD_SIZE;
    }

    clear_volatile(reply, SHMEM_RPMSG_BUF_SIZE);
    reply->src = SHMEM_EP_RTOS_UPPER;
    reply->dst = msg->src_ep;
    reply->reserved = 0;
    reply->flags = 0;
    reply->len = RPMSG_APP_HDR_SIZE + len;

    app = (volatile struct rpmsg_app_hdr *)reply->data;
    app->cmd = msg->cmd;
    app->cookie = msg->cookie;
    app->flags = 0;
    data = reply->data + RPMSG_APP_HDR_SIZE;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)msg->payload[i];

        if (c >= 'a' && c <= 'z') {
            c = (uint8_t)(c - ('a' - 'A'));
        }
        data[i] = c;
    }

    if (icc_message_send(reply) == 0) {
        icc_trace("rpmsg upper: reply sent cookie=%x\n",
                  (unsigned long)msg->cookie);
    }
}

void icc_bench_handler(struct icc_msg *msg)
{
    struct rpmsg_hdr *reply;
    volatile struct rpmsg_app_hdr *app;
    volatile uint8_t *data;
    uint32_t echo_len = msg->len;
    uint64_t t_rx = icc_rdtime();
    uint64_t t_tx;

    if (echo_len > SHMSG_PAYLOAD_SIZE - 16U) {
        echo_len = SHMSG_PAYLOAD_SIZE - 16U;
    }

    reply = icc_message_loan(msg->src_ep);
    if (reply == NULL) {
        debug_log("rpmsg bench: loan failed\n");
        return;
    }

    clear_volatile(reply, SHMEM_RPMSG_BUF_SIZE);
    reply->src = SHMEM_EP_RTOS_BENCH;
    reply->dst = msg->src_ep;
    reply->reserved = 0;
    reply->flags = 0;
    reply->len = RPMSG_APP_HDR_SIZE + 16U + echo_len;

    app = (volatile struct rpmsg_app_hdr *)reply->data;
    app->cmd = msg->cmd;
    app->cookie = msg->cookie;
    app->flags = 0;
    data = reply->data + RPMSG_APP_HDR_SIZE;

    for (uint32_t i = 0; i < echo_len; i++) {
        data[16U + i] = (uint8_t)msg->payload[i];
    }
    t_tx = icc_rdtime();
    for (uint32_t i = 0; i < 8U; i++) {
        data[i] = (uint8_t)(t_rx >> (i * 8U));
        data[8U + i] = (uint8_t)(t_tx >> (i * 8U));
    }

    (void)icc_message_send(reply);
}

void vIccTestTask(void *p_arg)
{
    struct rpmsg_hdr *msg;

    (void)p_arg;

    vTaskDelay(pdMS_TO_TICKS(9000));

    icc_announce_service("rpmsg-echo", SHMEM_EP_RTOS_ECHO);
    icc_announce_service("quardamp-rpc", SHMEM_EP_RTOS_UPPER);
    icc_announce_service("quardamp-bench", SHMEM_EP_RTOS_BENCH);

    msg = icc_message_loan(SHMEM_EP_XV6_TEST);
    if (msg == NULL) {
        debug_log("rpmsg test: loan failed\n");
        vTaskDelete(NULL);
        return;
    }

    icc_prepare_app_message(msg, SHMEM_EP_RTOS_ECHO, SHMEM_EP_XV6_TEST,
                            SHMEM_CMD_TEST, 0x4001, 0,
                            "rtos->xv6 rpmsg hello",
                            payload_len("rtos->xv6 rpmsg hello"));
    debug_log("rpmsg test: send to xv6 cookie=%x\n", (unsigned long)0x4001);

    (void)icc_message_send(msg);
    vTaskDelete(NULL);
}
