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

/*
 * Stage 4 的 FreeRTOS 侧 ICC 状态：
 * - xIccDispatchQueue：ISR 只把共享内存消息复制进队列，避免在中断里跑业务。
 * - xIccSendMutex：保护 to_xv6 ring 的 tail 分配，保证一次 loan/send 成对提交。
 * - icc_handlers：按 dst endpoint 分发，后续 rpmsg-like 服务可以继续注册新 EP。
 */
static QueueHandle_t xIccDispatchQueue;
static SemaphoreHandle_t xIccSendMutex;
static struct icc_handler_entry icc_handlers[ICC_MAX_HANDLERS];

static inline void icc_fence(void)
{
    __asm__ volatile ("fence rw, rw" ::: "memory");
}

static unsigned int payload_len(const char *s)
{
    unsigned int len = 0;

    while (s[len] != '\0') {
        len++;
    }
    return len;
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

static void fill_payload(volatile struct shmem_msg *msg, const char *payload)
{
    unsigned int len = payload_len(payload);

    if (len > sizeof(msg->payload)) {
        len = sizeof(msg->payload);
    }

    msg->len = len;
    for (unsigned int i = 0; i < len; i++) {
        msg->payload[i] = payload[i];
    }
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

void icc_init(void)
{
    xIccDispatchQueue = xQueueCreate(ICC_DISPATCH_QUEUE_DEPTH,
                                     sizeof(struct shmem_msg));
    xIccSendMutex = xSemaphoreCreateMutex();

    for (int i = 0; i < ICC_MAX_HANDLERS; i++) {
        icc_handlers[i].ep = 0;
        icc_handlers[i].handler = NULL;
    }

    if (xIccDispatchQueue == NULL || xIccSendMutex == NULL) {
        debug_log("icc init failed: queue=%p mutex=%p\n",
                  (unsigned long)xIccDispatchQueue,
                  (unsigned long)xIccSendMutex);
        return;
    }

    debug_log("icc init: queue depth=%d handlers=%d\n",
              ICC_DISPATCH_QUEUE_DEPTH, ICC_MAX_HANDLERS);
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
            debug_log("icc register: ep=%x slot=%d\n",
                      (unsigned long)ep, i);
            return 0;
        }
    }

    debug_log("icc register failed: table full ep=%x\n", (unsigned long)ep);
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
        uint32_t head = SHMEM_CTRL_BASE->to_rtos_head;
        uint32_t tail = SHMEM_CTRL_BASE->to_rtos_tail;
        uint32_t idx;
        struct shmem_msg msg;

        if (head == tail) {
            break;
        }

        idx = head % SHMEM_RING_SIZE;
        /*
         * ISR 里复制整包，而不是把共享内存 slot 指针投进队列：
         * head 前移后该 slot 就重新归 xv6 所有，任务上下文不能再依赖它。
         */
        icc_fence();
        copy_msg_from_volatile(&msg, &SHMEM_TO_RTOS_BASE[idx]);

        icc_fence();
        SHMEM_CTRL_BASE->to_rtos_head = head + 1;

        if (xQueueSendFromISR(xIccDispatchQueue, &msg,
                              &xHigherPriorityTaskWoken) != pdTRUE) {
            debug_log("icc isr: dispatch queue full, drop cookie=%x\n",
                      (unsigned long)msg.cookie);
        }
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

struct shmem_msg *icc_message_loan(uint32_t dst_ep)
{
    uint32_t head;
    uint32_t tail;
    uint32_t idx;
    volatile struct shmem_msg *slot;

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

    /*
     * loan 成功后，调用者独占当前 tail slot。
     * 只有 icc_message_send() 发布 tail 或 icc_message_release() 放弃后，
     * 其他任务才可以继续申请 to_xv6 发送槽。
     */
    head = SHMEM_CTRL_BASE->to_xv6_head;
    tail = SHMEM_CTRL_BASE->to_xv6_tail;
    if (tail - head >= SHMEM_RING_SIZE) {
        debug_log("icc loan failed: to_xv6 full head=%d tail=%d\n",
                  (int)head, (int)tail);
        xSemaphoreGive(xIccSendMutex);
        return NULL;
    }

    idx = tail % SHMEM_RING_SIZE;
    slot = &SHMEM_TO_XV6_BASE[idx];
    clear_volatile_msg(slot);
    slot->src_ep = SHMEM_EP_RTOS_ECHO;
    slot->dst_ep = dst_ep;
    slot->cmd = SHMEM_CMD_TEST;
    slot->flags = 0;

    return (struct shmem_msg *)slot;
}

int icc_message_send(struct shmem_msg *msg)
{
    uint32_t tail = SHMEM_CTRL_BASE->to_xv6_tail;
    uint32_t idx = tail % SHMEM_RING_SIZE;
    volatile struct shmem_msg *slot = &SHMEM_TO_XV6_BASE[idx];

    /*
     * 只允许提交刚 loan 出来的 tail slot，避免误把历史 slot 或栈上消息
     * 当成共享内存消息发布给 xv6。
     */
    if (msg == NULL) {
        return -1;
    }
    if ((volatile struct shmem_msg *)msg != slot) {
        debug_log("icc send failed: msg=%p expected=%p\n",
                  (unsigned long)msg, (unsigned long)slot);
        xSemaphoreGive(xIccSendMutex);
        return -1;
    }

    icc_fence();
    SHMEM_CTRL_BASE->to_xv6_tail = tail + 1;
    mailbox_ring_to_xv6(SHMEM_DOORBELL_CH0);
    xSemaphoreGive(xIccSendMutex);
    return 0;
}

void icc_message_release(struct shmem_msg *msg)
{
    (void)msg;

    if (xIccSendMutex != NULL) {
        xSemaphoreGive(xIccSendMutex);
    }
}

void vIccDispatchTask(void *p_arg)
{
    struct shmem_msg msg;

    (void)p_arg;

    for (;;) {
        if (xQueueReceive(xIccDispatchQueue, &msg, portMAX_DELAY) == pdTRUE) {
            int found = 0;

            /*
             * 目前 handler 表很小，线性查找足够清楚；阶段 5 若 endpoint
             * 数量变多，再替换为哈希或静态索引表。
             */
            for (int i = 0; i < ICC_MAX_HANDLERS; i++) {
                if (icc_handlers[i].handler != NULL &&
                    icc_handlers[i].ep == msg.dst_ep) {
                    icc_handlers[i].handler(&msg);
                    found = 1;
                    break;
                }
            }

            if (!found) {
                debug_log("icc dispatch: no handler for dst=%x cookie=%x\n",
                          (unsigned long)msg.dst_ep,
                          (unsigned long)msg.cookie);
            }
        }
    }
}

void icc_echo_handler(struct shmem_msg *msg)
{
    struct shmem_msg *reply;
    char payload[sizeof(msg->payload) + 1];

    payload_to_string(payload, sizeof(payload), msg);
    debug_log("icc echo: rx src=%x dst=%x cmd=%x cookie=%x payload=%s\n",
              (unsigned long)msg->src_ep, (unsigned long)msg->dst_ep,
              (unsigned long)msg->cmd, (unsigned long)msg->cookie, payload);

    reply = icc_message_loan(msg->src_ep);
    if (reply == NULL) {
        debug_log("icc echo: loan failed\n");
        return;
    }

    reply->cmd = msg->cmd;
    reply->cookie = msg->cookie;
    reply->flags = 0;
    reply->src_ep = SHMEM_EP_RTOS_ECHO;
    fill_payload((volatile struct shmem_msg *)reply, "rtos->xv6 phase6 ack");

    (void)icc_message_send(reply);
}

void icc_upper_handler(struct shmem_msg *msg)
{
    struct shmem_msg *reply;
    char payload[sizeof(msg->payload) + 1];
    unsigned int len = msg->len;

    payload_to_string(payload, sizeof(payload), msg);
    debug_log("icc upper: rx src=%x dst=%x cmd=%x cookie=%x payload=%s\n",
              (unsigned long)msg->src_ep, (unsigned long)msg->dst_ep,
              (unsigned long)msg->cmd, (unsigned long)msg->cookie, payload);

    reply = icc_message_loan(msg->src_ep);
    if (reply == NULL) {
        debug_log("icc upper: loan failed\n");
        return;
    }

    if (len > sizeof(reply->payload)) {
        len = sizeof(reply->payload);
    }

    /*
     * Keep the service deliberately tiny and deterministic: ASCII lower-case
     * bytes are converted in-place, all other bytes are returned unchanged.
     * The cookie is copied from the request so xv6 can match the RPC pending
     * slot without relying on endpoint order.
     */
    for (unsigned int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)msg->payload[i];

        if (c >= 'a' && c <= 'z') {
            c = (uint8_t)(c - ('a' - 'A'));
        }
        reply->payload[i] = (char)c;
    }

    reply->src_ep = SHMEM_EP_RTOS_UPPER;
    reply->cmd = msg->cmd;
    reply->cookie = msg->cookie;
    reply->flags = 0;
    reply->len = len;

    if (icc_message_send(reply) == 0) {
        debug_log("icc upper: reply sent cookie=%x\n",
                  (unsigned long)msg->cookie);
    }
}

void vIccTestTask(void *p_arg)
{
    struct shmem_msg *msg;

    (void)p_arg;

    /*
     * 等 xv6 完成 shmem_init() 和 PLIC mailbox 使能后再主动发送。
     * 这是阶段 4 的 FreeRTOS -> xv6 主动消息冒烟，不依赖用户输入。
     */
    vTaskDelay(pdMS_TO_TICKS(9000));

    msg = icc_message_loan(SHMEM_EP_XV6_TEST);
    if (msg == NULL) {
        debug_log("icc test: loan failed\n");
        vTaskDelete(NULL);
        return;
    }

    msg->cmd = SHMEM_CMD_TEST;
    msg->cookie = 0x4001;
    msg->flags = 0;
    msg->src_ep = SHMEM_EP_RTOS_ECHO;
    fill_payload((volatile struct shmem_msg *)msg, "rtos->xv6 phase6 hello");
    debug_log("icc test: send to xv6 cookie=%x\n", (unsigned long)msg->cookie);

    (void)icc_message_send(msg);

    vTaskDelete(NULL);
}
