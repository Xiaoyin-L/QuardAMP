#include <stdint.h>
#include "debug_log.h"
#include "mailbox.h"
#include "shmem.h"

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

static inline void shmem_fence(void)
{
    __asm__ volatile ("fence rw, rw" ::: "memory");
}

static volatile struct shmem_ctrl *shmem_ctrl(void)
{
    return (volatile struct shmem_ctrl *)(SHMEM_ADDR + SHMEM_CTRL_OFFSET);
}

static volatile struct shmem_msg *to_rtos_ring(void)
{
    return (volatile struct shmem_msg *)(SHMEM_ADDR + SHMEM_TO_RTOS_OFFSET);
}

static volatile struct shmem_msg *to_xv6_ring(void)
{
    return (volatile struct shmem_msg *)(SHMEM_ADDR + SHMEM_TO_XV6_OFFSET);
}

static void copy_msg_from_volatile(struct shmem_msg *dst,
                                   volatile struct shmem_msg *src)
{
    uint8_t *d = (uint8_t *)dst;
    volatile uint8_t *s = (volatile uint8_t *)src;

    for (unsigned int i = 0; i < sizeof(*dst); i++) {
        d[i] = s[i];
    }
}

static void clear_volatile_msg(volatile struct shmem_msg *msg)
{
    volatile uint8_t *p = (volatile uint8_t *)msg;

    for (unsigned int i = 0; i < sizeof(*msg); i++) {
        p[i] = 0;
    }
}

static unsigned int payload_len(const char *s)
{
    unsigned int len = 0;

    while (s[len] != '\0') {
        len++;
    }
    return len;
}

static int payload_matches(const struct shmem_msg *msg, const char *expected)
{
    unsigned int expected_len = payload_len(expected);

    if (msg->len != expected_len) {
        return 0;
    }

    for (unsigned int i = 0; i < expected_len; i++) {
        if (msg->payload[i] != expected[i]) {
            return 0;
        }
    }
    return 1;
}

static void payload_to_string(char *dst, unsigned int dst_size,
                              const struct shmem_msg *msg)
{
    unsigned int len = msg->len;

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

static int send_ack_to_xv6(const struct shmem_msg *req)
{
    volatile struct shmem_ctrl *ctrl = shmem_ctrl();
    volatile struct shmem_msg *ring = to_xv6_ring();
    volatile struct shmem_msg *ack;
    const char payload[] = "rtos->xv6 phase3 ack";
    uint32_t head = ctrl->to_xv6_head;
    uint32_t tail = ctrl->to_xv6_tail;
    uint32_t idx;
    unsigned int len = payload_len(payload);

    if (tail - head >= SHMEM_RING_SIZE) {
        debug_log("shmem: to_xv6 ring full head=%d tail=%d\n",
                  (int)head, (int)tail);
        return -1;
    }

    idx = tail % SHMEM_RING_SIZE;
    ack = &ring[idx];
    clear_volatile_msg(ack);

    ack->src_ep = SHMEM_EP_RTOS_ECHO;
    ack->dst_ep = req->src_ep;
    ack->cmd = req->cmd;
    ack->len = len;
    ack->cookie = req->cookie;
    ack->flags = 0;

    for (unsigned int i = 0; i < len; i++) {
        ack->payload[i] = payload[i];
    }

    shmem_fence();
    ctrl->to_xv6_tail = tail + 1;

    debug_log("shmem: ack to_xv6 idx=%d cookie=%x tail=%d\n",
              (int)idx, (unsigned long)req->cookie, (int)(tail + 1));
    mailbox_ring_to_xv6(SHMEM_DOORBELL_CH0);
    return 0;
}

void shmem_handle_to_rtos_doorbell(void)
{
    volatile struct shmem_ctrl *ctrl = shmem_ctrl();
    volatile struct shmem_msg *ring = to_rtos_ring();
    struct shmem_msg msg;
    char payload[sizeof(msg.payload) + 1];

    if (ctrl->magic != SHMEM_MAGIC || ctrl->version != SHMEM_VERSION) {
        debug_log("shmem: not ready magic=%x version=%d\n",
                  (unsigned long)ctrl->magic, (int)ctrl->version);
        return;
    }

    for (;;) {
        uint32_t head = ctrl->to_rtos_head;
        uint32_t tail = ctrl->to_rtos_tail;
        uint32_t idx;
        int verify_ok;

        if (head == tail) {
            return;
        }

        shmem_fence();
        idx = head % SHMEM_RING_SIZE;
        copy_msg_from_volatile(&msg, &ring[idx]);

        payload_to_string(payload, sizeof(payload), &msg);
        verify_ok = payload_matches(&msg, "xv6->rtos phase3 hello");
        debug_log("shmem: recv to_rtos idx=%d src=%x dst=%x cmd=%x len=%d cookie=%x flags=%x payload=%s verify=%s\n",
                  (int)idx, (unsigned long)msg.src_ep,
                  (unsigned long)msg.dst_ep, (unsigned long)msg.cmd,
                  (int)msg.len, (unsigned long)msg.cookie,
                  (unsigned long)msg.flags, payload,
                  verify_ok ? "ok" : "FAIL");

        shmem_fence();
        ctrl->to_rtos_head = head + 1;

        if (send_ack_to_xv6(&msg) != 0) {
            return;
        }
    }
}
