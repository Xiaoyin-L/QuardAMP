#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "shmem.h"
#include "proc.h"

/*
 * icc.c -- xv6-side rpmsg transport over a private mailbox.
 *
 * The wire format is now intentionally close to Linux virtio-rpmsg:
 *   - two virtio split rings, one per direction;
 *   - each descriptor points to a 512-byte rpmsg buffer;
 *   - struct rpmsg_hdr is the transport header;
 *   - QuardAMP cmd/cookie live inside rpmsg_hdr.data as application payload.
 *
 * The public syscall shape is kept so existing tests still work while the
 * shared-memory ABI moves away from the old custom shmem_msg slots.
 */
#define ICC_MAX_ENDPOINTS 8
#define ICC_RPC_MAX_PENDING 4
#define ICC_EP_QUEUE_DEPTH 8
#define ICC_TICK_MS 100

#ifndef ICC_VERBOSE
#define ICC_VERBOSE 0
#endif

#if ICC_VERBOSE
#define icc_trace(...) printf(__VA_ARGS__)
#else
#define icc_trace(...) do { } while(0)
#endif

struct icc_rx_item {
  uint16 desc_id;
  uint32 wire_len;
  volatile uint8 *buf;
  struct icc_msg msg;
};

struct icc_endpoint {
  uint32 ep;
  uint32 head;
  uint32 count;
  uint32 drops;
  uint32 deadline;
  struct icc_rx_item queue[ICC_EP_QUEUE_DEPTH];
  struct spinlock lock;
};

struct icc_rpc_pending {
  uint32 cookie;
  uint32 deadline;
  int in_use;
  int has_reply;
  struct icc_msg reply;
  struct spinlock lock;
};

static struct icc_endpoint icc_eps[ICC_MAX_ENDPOINTS];
static struct spinlock icc_send_lock;
static struct icc_rpc_pending icc_rpc_table[ICC_RPC_MAX_PENDING];
static struct spinlock icc_rpc_lock;
static uint32 icc_rpc_next_cookie = 0x6000;
static uint16 icc_to_xv6_last_avail;
static uint16 icc_to_rtos_last_used;
static uint16 icc_to_rtos_free[SHMSG_SLOT_NUM];
static uint16 icc_to_rtos_free_count;

static uint32
icc_now_ticks(void)
{
  uint32 now;

  acquire(&tickslock);
  now = ticks;
  release(&tickslock);
  return now;
}

static uint32
icc_timeout_to_ticks(uint32 timeout_ms)
{
  uint32 timeout_ticks;

  if(timeout_ms == 0)
    return 0;
  timeout_ticks = (timeout_ms + ICC_TICK_MS - 1) / ICC_TICK_MS;
  if(timeout_ticks == 0)
    timeout_ticks = 1;
  return timeout_ticks;
}

static int
icc_deadline_expired(uint32 deadline)
{
  if(deadline == 0)
    return 0;
  return (int)(icc_now_ticks() - deadline) >= 0;
}

static struct icc_endpoint*
icc_find_ep(uint32 ep)
{
  for(int i = 0; i < ICC_MAX_ENDPOINTS; i++){
    if(icc_eps[i].ep == ep)
      return &icc_eps[i];
  }
  return 0;
}

static void
clear_volatile(volatile void *ptr, uint n)
{
  volatile uint8 *p = (volatile uint8*)ptr;

  for(uint i = 0; i < n; i++)
    p[i] = 0;
}

static void
icc_rx_release_to_xv6(uint16 desc_id, uint32 len)
{
  uint16 used_idx = SHMEM_TO_XV6_USED->idx;

  SHMEM_TO_XV6_USED->ring[used_idx % SHMSG_SLOT_NUM].id = desc_id;
  SHMEM_TO_XV6_USED->ring[used_idx % SHMSG_SLOT_NUM].len = len;
  __sync_synchronize();
  SHMEM_TO_XV6_USED->idx = used_idx + 1;
  __sync_synchronize();
}

static void
icc_tx_free_init(void)
{
  icc_to_rtos_last_used = 0;
  icc_to_rtos_free_count = SHMSG_SLOT_NUM;
  for(int i = 0; i < SHMSG_SLOT_NUM; i++)
    icc_to_rtos_free[i] = i;
}

/*
 * Virtio split rings transfer descriptor ownership through the used ring:
 * sender owns ids in its free list, receiver returns consumed ids in used[].
 * Reclaim all newly used descriptors before checking for transmit space.
 */
static void
icc_tx_reclaim_locked(void)
{
  while(icc_to_rtos_last_used != SHMEM_TO_RTOS_USED->idx){
    uint16 id = SHMEM_TO_RTOS_USED->ring[
      icc_to_rtos_last_used % SHMSG_SLOT_NUM].id;

    if(id < SHMSG_SLOT_NUM && icc_to_rtos_free_count < SHMSG_SLOT_NUM)
      icc_to_rtos_free[icc_to_rtos_free_count++] = id;
    icc_to_rtos_last_used++;
  }
}

static int
rpmsg_decode_app(volatile uint8 *wire, uint32 wire_len, struct icc_msg *out,
                 int copy_payload)
{
  volatile struct rpmsg_hdr *hdr = (volatile struct rpmsg_hdr*)wire;
  volatile struct rpmsg_app_hdr *app;
  uint32 payload_len;
  uint16 hdr_len;

  if(wire_len < RPMSG_HDR_SIZE)
    return -1;
  hdr_len = hdr->len;
  if(hdr_len < RPMSG_APP_HDR_SIZE)
    return -1;
  if(RPMSG_HDR_SIZE + hdr_len > wire_len)
    return -1;

  app = (volatile struct rpmsg_app_hdr*)hdr->data;
  payload_len = hdr_len - RPMSG_APP_HDR_SIZE;
  if(payload_len > SHMSG_PAYLOAD_SIZE)
    payload_len = SHMSG_PAYLOAD_SIZE;

  out->src_ep = hdr->src;
  out->dst_ep = hdr->dst;
  out->cmd = app->cmd;
  out->cookie = app->cookie;
  out->flags = app->flags;
  out->len = payload_len;
  if(copy_payload){
    volatile uint8 *payload = hdr->data + RPMSG_APP_HDR_SIZE;

    for(uint32 i = 0; i < payload_len; i++)
      out->payload[i] = payload[i];
  }
  return 0;
}

static void
rpmsg_handle_ns(volatile uint8 *wire, uint32 wire_len)
{
  volatile struct rpmsg_hdr *hdr = (volatile struct rpmsg_hdr*)wire;
  volatile struct rpmsg_ns_msg *ns;
  char name[RPMSG_NAME_SIZE + 1];
  uint32 len;

  if(wire_len < RPMSG_HDR_SIZE || hdr->len < sizeof(struct rpmsg_ns_msg))
    return;

  ns = (volatile struct rpmsg_ns_msg*)hdr->data;
  len = RPMSG_NAME_SIZE;
  for(uint32 i = 0; i < len; i++)
    name[i] = ns->name[i];
  name[RPMSG_NAME_SIZE] = '\0';
  printf("rpmsg ns: %s addr=%x flags=%x\n", name, ns->addr, ns->flags);
}

static volatile uint8*
rpmsg_payload(volatile uint8 *wire)
{
  volatile struct rpmsg_hdr *hdr = (volatile struct rpmsg_hdr*)wire;

  return hdr->data + RPMSG_APP_HDR_SIZE;
}

static void
rpmsg_make_app(volatile uint8 *buf, uint32 src, uint32 dst, uint32 cmd,
               const char *payload, uint32 len, uint32 cookie,
               uint32 *wire_len)
{
  volatile struct rpmsg_hdr *hdr = (volatile struct rpmsg_hdr*)buf;
  volatile struct rpmsg_app_hdr *app;
  volatile uint8 *data;
  uint32 copy_len;

  if(len > SHMSG_PAYLOAD_SIZE)
    len = SHMSG_PAYLOAD_SIZE;

  clear_volatile(buf, SHMEM_RPMSG_BUF_SIZE);

  hdr->src = src;
  hdr->dst = dst;
  hdr->reserved = 0;
  hdr->flags = 0;
  hdr->len = RPMSG_APP_HDR_SIZE + len;

  app = (volatile struct rpmsg_app_hdr*)hdr->data;
  app->cmd = cmd;
  app->cookie = cookie;
  app->flags = 0;

  data = hdr->data + RPMSG_APP_HDR_SIZE;
  for(copy_len = 0; copy_len < len; copy_len++)
    data[copy_len] = payload[copy_len];

  *wire_len = RPMSG_HDR_SIZE + hdr->len;
}

static struct icc_rpc_pending*
icc_rpc_alloc(void)
{
  struct icc_rpc_pending *p = 0;

  acquire(&icc_rpc_lock);
  for(int i = 0; i < ICC_RPC_MAX_PENDING; i++){
    if(!icc_rpc_table[i].in_use){
      p = &icc_rpc_table[i];
      acquire(&p->lock);
      p->in_use = 1;
      p->has_reply = 0;
      p->cookie = icc_rpc_next_cookie++;
      p->deadline = 0;
      memset(&p->reply, 0, sizeof(p->reply));
      release(&p->lock);
      break;
    }
  }
  release(&icc_rpc_lock);

  return p;
}

static void
icc_rpc_free(struct icc_rpc_pending *p)
{
  acquire(&icc_rpc_lock);
  acquire(&p->lock);
  p->in_use = 0;
  p->has_reply = 0;
  p->cookie = 0;
  p->deadline = 0;
  memset(&p->reply, 0, sizeof(p->reply));
  release(&p->lock);
  release(&icc_rpc_lock);
}

static int
icc_rpc_try_match(struct icc_msg *msg, volatile uint8 *payload)
{
  for(int i = 0; i < ICC_RPC_MAX_PENDING; i++){
    struct icc_rpc_pending *p = &icc_rpc_table[i];

    acquire(&p->lock);
    if(p->in_use && p->cookie == msg->cookie){
      p->reply = *msg;
      for(uint32 j = 0; j < msg->len; j++)
        p->reply.payload[j] = payload[j];
      p->has_reply = 1;
      p->deadline = 0;
      wakeup(p);
      release(&p->lock);

      icc_trace("rpmsg rx: rpc reply matched cookie=%x\n", msg->cookie);
      return 1;
    }
    release(&p->lock);
  }

  return 0;
}

void
icc_init(void)
{
  for(int i = 0; i < ICC_MAX_ENDPOINTS; i++){
    icc_eps[i].ep = 0;
    icc_eps[i].head = 0;
    icc_eps[i].count = 0;
    icc_eps[i].drops = 0;
    icc_eps[i].deadline = 0;
    initlock(&icc_eps[i].lock, "icc_ep");
  }

  icc_eps[0].ep = SHMEM_EP_XV6_TEST;
  initlock(&icc_send_lock, "icc_send");

  for(int i = 0; i < ICC_RPC_MAX_PENDING; i++){
    icc_rpc_table[i].cookie = 0;
    icc_rpc_table[i].deadline = 0;
    icc_rpc_table[i].in_use = 0;
    icc_rpc_table[i].has_reply = 0;
    memset(&icc_rpc_table[i].reply, 0, sizeof(icc_rpc_table[i].reply));
    initlock(&icc_rpc_table[i].lock, "icc_rpc");
  }
  initlock(&icc_rpc_lock, "icc_rpc_tbl");
  icc_to_xv6_last_avail = 0;
  icc_tx_free_init();

  printf("rpmsg: init endpoints=%d local_ep=%x buf=%d payload=%d\n",
         ICC_MAX_ENDPOINTS, SHMEM_EP_XV6_TEST,
         SHMEM_RPMSG_BUF_SIZE, SHMSG_PAYLOAD_SIZE);
}

void
icc_notify_recv(uint32 reason)
{
  if((reason & SHMEM_DOORBELL_VRING_TO_XV6) == 0 &&
     reason != SHMEM_DOORBELL_VRING_TO_RTOS)
    return;

  if(SHMEM_CTRL_BASE->magic != SHMEM_MAGIC ||
     SHMEM_CTRL_BASE->version != SHMEM_VERSION)
    return;

  for(;;){
    uint16 avail_idx = SHMEM_TO_XV6_AVAIL->idx;
    uint16 desc_id;
    uint32 wire_len;
    uint32 tail;
    struct icc_msg msg;
    struct icc_rx_item item;
    struct icc_endpoint *e;
    volatile struct vring_desc *desc;
    volatile uint8 *buf;

    if(icc_to_xv6_last_avail == avail_idx)
      break;

    desc_id = SHMEM_TO_XV6_AVAIL->ring[icc_to_xv6_last_avail % SHMSG_SLOT_NUM];
    if(desc_id >= SHMSG_SLOT_NUM){
      icc_to_xv6_last_avail++;
      continue;
    }

    desc = &SHMEM_TO_XV6_DESC[desc_id];
    wire_len = desc->len;
    if(wire_len > SHMEM_RPMSG_BUF_SIZE)
      wire_len = SHMEM_RPMSG_BUF_SIZE;
    buf = (volatile uint8*)desc->addr;

    __sync_synchronize();
    icc_to_xv6_last_avail++;

    if(((volatile struct rpmsg_hdr*)buf)->dst == RPMSG_NS_ADDR){
      rpmsg_handle_ns(buf, wire_len);
      icc_rx_release_to_xv6(desc_id, wire_len);
      continue;
    }

    if(rpmsg_decode_app(buf, wire_len, &msg, 0) < 0){
      printf("rpmsg rx: bad buffer len=%d\n", wire_len);
      icc_rx_release_to_xv6(desc_id, wire_len);
      continue;
    }

    if(icc_rpc_try_match(&msg, rpmsg_payload(buf))){
      icc_rx_release_to_xv6(desc_id, wire_len);
      continue;
    }

    e = icc_find_ep(msg.dst_ep);
    if(e != 0){
      acquire(&e->lock);
      if(e->count == ICC_EP_QUEUE_DEPTH){
        e->drops++;
        printf("rpmsg rx: queue full ep=%x drops=%d cookie=%x\n",
               msg.dst_ep, e->drops, msg.cookie);
        release(&e->lock);
        icc_rx_release_to_xv6(desc_id, wire_len);
        continue;
      }
      tail = (e->head + e->count) % ICC_EP_QUEUE_DEPTH;
      item.desc_id = desc_id;
      item.wire_len = wire_len;
      item.buf = buf;
      item.msg = msg;
      e->queue[tail] = item;
      e->count++;
      e->deadline = 0;
      wakeup(e);
      release(&e->lock);

      icc_trace("rpmsg rx: ep=%x src=%x cmd=%x cookie=%x queued desc=%d\n",
                msg.dst_ep, msg.src_ep, msg.cmd, msg.cookie, desc_id);
    } else {
      printf("rpmsg rx: no endpoint dst=%x src=%x cmd=%x cookie=%x\n",
             msg.dst_ep, msg.src_ep, msg.cmd, msg.cookie);
      icc_rx_release_to_xv6(desc_id, wire_len);
    }
  }
}

static int
icc_ready(void)
{
  return SHMEM_CTRL_BASE->magic == SHMEM_MAGIC &&
         SHMEM_CTRL_BASE->version == SHMEM_VERSION;
}

static int
icc_tx_loan_locked(uint16 *desc_id, volatile uint8 **buf)
{
  uint16 avail_idx;

  icc_tx_reclaim_locked();
  avail_idx = SHMEM_TO_RTOS_AVAIL->idx;
  if(icc_to_rtos_free_count == 0){
    printf("rpmsg tx: to_rtos free list empty avail=%d used=%d\n",
           avail_idx, SHMEM_TO_RTOS_USED->idx);
    return -1;
  }

  *desc_id = icc_to_rtos_free[--icc_to_rtos_free_count];
  *buf = SHMEM_TO_RTOS_BUF_BASE + *desc_id * SHMEM_RPMSG_BUF_SIZE;
  return 0;
}

static void
icc_tx_commit_locked(uint16 desc_id, uint32 wire_len)
{
  uint16 avail_idx = SHMEM_TO_RTOS_AVAIL->idx;
  volatile uint8 *buf = SHMEM_TO_RTOS_BUF_BASE +
                        desc_id * SHMEM_RPMSG_BUF_SIZE;

  SHMEM_TO_RTOS_DESC[desc_id].addr = (uint64)buf;
  SHMEM_TO_RTOS_DESC[desc_id].len = wire_len;
  SHMEM_TO_RTOS_DESC[desc_id].flags = 0;
  SHMEM_TO_RTOS_DESC[desc_id].next = 0;

  SHMEM_TO_RTOS_AVAIL->ring[avail_idx % SHMSG_SLOT_NUM] = desc_id;
  __sync_synchronize();
  SHMEM_TO_RTOS_AVAIL->idx = avail_idx + 1;
  __sync_synchronize();
}

static int
icc_send_locked(uint32 dst_ep, uint32 cmd, const char *payload, uint32 len,
                uint32 cookie)
{
  uint16 desc_id;
  uint32 wire_len;
  volatile uint8 *buf;

  /*
   * Keep the xv6 TX side in the same shape as rpmsg: claim one outbound
   * buffer, fill the rpmsg header in-place, then publish its descriptor.
   */
  if(icc_tx_loan_locked(&desc_id, &buf) < 0)
    return -1;

  rpmsg_make_app(buf, SHMEM_EP_XV6_TEST, dst_ep, cmd, payload, len, cookie,
                 &wire_len);
  icc_tx_commit_locked(desc_id, wire_len);
  icc_trace("rpmsg tx: desc=%d dst=%x cmd=%x cookie=%x len=%d -> FreeRTOS\n",
            desc_id, dst_ep, cmd, cookie, len);
  return 0;
}

int
icc_send(uint32 dst_ep, uint32 cmd, const char *payload, uint32 len,
         uint32 cookie)
{
  int ret;

  if(!icc_ready()){
    printf("rpmsg tx: shmem not ready magic=%x version=%d\n",
           SHMEM_CTRL_BASE->magic, SHMEM_CTRL_BASE->version);
    return -1;
  }

  if(len > SHMSG_PAYLOAD_SIZE)
    len = SHMSG_PAYLOAD_SIZE;

  acquire(&icc_send_lock);
  ret = icc_send_locked(dst_ep, cmd, payload, len, cookie);
  release(&icc_send_lock);

  if(ret < 0)
    return -1;

  mailbox_ring_to_rtos(SHMEM_DOORBELL_VRING_TO_RTOS);
  return 0;
}

int
icc_send_batch(uint32 dst_ep, uint32 cmd, const char *payload, uint32 len,
               uint32 base_cookie, uint32 count)
{
  uint32 sent = 0;

  if(!icc_ready()){
    printf("rpmsg tx batch: shmem not ready magic=%x version=%d\n",
           SHMEM_CTRL_BASE->magic, SHMEM_CTRL_BASE->version);
    return -1;
  }
  if(len > SHMSG_PAYLOAD_SIZE)
    len = SHMSG_PAYLOAD_SIZE;
  if(count > SHMSG_SLOT_NUM)
    count = SHMSG_SLOT_NUM;

  acquire(&icc_send_lock);
  while(sent < count){
    if(icc_send_locked(dst_ep, cmd, payload, len, base_cookie + sent) < 0)
      break;
    sent++;
  }
  release(&icc_send_lock);

  /*
   * Batch mode deliberately publishes N descriptors but sends one doorbell.
   * The peer ISR drains avail.idx until it catches up, matching virtqueue
   * notification coalescing without changing the per-buffer rpmsg format.
   */
  if(sent != 0)
    mailbox_ring_to_rtos(SHMEM_DOORBELL_VRING_TO_RTOS);

  return sent == 0 ? -1 : (int)sent;
}

int
icc_recv(uint32 ep, struct icc_msg *out, uint32 timeout_ms)
{
  struct icc_endpoint *e = icc_find_ep(ep);
  struct icc_rx_item item;
  uint32 timeout_ticks;
  uint32 deadline = 0;

  if(e == 0)
    return -1;

  timeout_ticks = icc_timeout_to_ticks(timeout_ms);
  if(timeout_ticks != 0)
    deadline = icc_now_ticks() + timeout_ticks;

  acquire(&e->lock);
  e->deadline = deadline;
  while(e->count == 0 && !killed(myproc())){
    if(icc_deadline_expired(deadline)){
      e->deadline = 0;
      release(&e->lock);
      return -1;
    }
    sleep(e, &e->lock);
  }

  if(killed(myproc())){
    e->deadline = 0;
    release(&e->lock);
    return -1;
  }

  item = e->queue[e->head];
  e->head = (e->head + 1) % ICC_EP_QUEUE_DEPTH;
  e->count--;
  if(e->count != 0)
    wakeup(e);
  e->deadline = 0;
  release(&e->lock);

  *out = item.msg;
  for(uint32 i = 0; i < out->len; i++)
    out->payload[i] = rpmsg_payload(item.buf)[i];
  icc_rx_release_to_xv6(item.desc_id, item.wire_len);
  return 0;
}

int
icc_recv_user(uint32 ep, pagetable_t pagetable, uint64 buf_addr,
              uint32 max_len, uint32 timeout_ms)
{
  struct icc_endpoint *e = icc_find_ep(ep);
  struct icc_rx_item item;
  uint32 timeout_ticks;
  uint32 deadline = 0;
  uint32 copy_len;
  int ret = 0;

  if(e == 0)
    return -1;

  timeout_ticks = icc_timeout_to_ticks(timeout_ms);
  if(timeout_ticks != 0)
    deadline = icc_now_ticks() + timeout_ticks;

  acquire(&e->lock);
  e->deadline = deadline;
  while(e->count == 0 && !killed(myproc())){
    if(icc_deadline_expired(deadline)){
      e->deadline = 0;
      release(&e->lock);
      return -1;
    }
    sleep(e, &e->lock);
  }

  if(killed(myproc())){
    e->deadline = 0;
    release(&e->lock);
    return -1;
  }

  item = e->queue[e->head];
  e->head = (e->head + 1) % ICC_EP_QUEUE_DEPTH;
  e->count--;
  if(e->count != 0)
    wakeup(e);
  e->deadline = 0;
  release(&e->lock);

  copy_len = item.msg.len;
  if(copy_len > max_len)
    copy_len = max_len;
  if(copy_len > SHMSG_PAYLOAD_SIZE)
    copy_len = SHMSG_PAYLOAD_SIZE;

  if(copy_len != 0 &&
     copyout(pagetable, buf_addr, (char*)rpmsg_payload(item.buf),
             (uint64)copy_len) < 0)
    ret = -1;

  icc_rx_release_to_xv6(item.desc_id, item.wire_len);
  return ret < 0 ? -1 : (int)copy_len;
}

int
icc_rpc_call(uint32 dst_ep, uint32 cmd, const char *payload, uint32 len,
             char *reply_buf, uint32 reply_max, uint32 *reply_len,
             uint32 timeout_ms)
{
  struct icc_rpc_pending *p;
  uint32 cookie;
  uint32 copy_len;
  uint32 timeout_ticks;
  uint32 deadline = 0;

  if(reply_len == 0)
    return -1;

  if(len > SHMSG_PAYLOAD_SIZE)
    len = SHMSG_PAYLOAD_SIZE;
  if(reply_max > SHMSG_PAYLOAD_SIZE)
    reply_max = SHMSG_PAYLOAD_SIZE;
  timeout_ticks = icc_timeout_to_ticks(timeout_ms);
  if(timeout_ticks != 0)
    deadline = icc_now_ticks() + timeout_ticks;

  p = icc_rpc_alloc();
  if(p == 0){
    printf("rpmsg rpc: pending table full\n");
    return -1;
  }

  acquire(&p->lock);
  cookie = p->cookie;
  p->deadline = deadline;
  release(&p->lock);

  if(icc_send(dst_ep, cmd, payload, len, cookie) < 0){
    icc_rpc_free(p);
    return -1;
  }

  acquire(&p->lock);
  while(!p->has_reply && !killed(myproc())){
    if(icc_deadline_expired(deadline)){
      release(&p->lock);
      icc_rpc_free(p);
      return -1;
    }
    sleep(p, &p->lock);
  }

  if(killed(myproc())){
    release(&p->lock);
    icc_rpc_free(p);
    return -1;
  }

  copy_len = p->reply.len;
  if(copy_len > reply_max)
    copy_len = reply_max;
  if(copy_len > SHMSG_PAYLOAD_SIZE)
    copy_len = SHMSG_PAYLOAD_SIZE;

  if(copy_len > 0)
    memmove(reply_buf, p->reply.payload, copy_len);
  *reply_len = copy_len;
  release(&p->lock);

  icc_rpc_free(p);
  return 0;
}

void
icc_timeout_poll(void)
{
  for(int i = 0; i < ICC_MAX_ENDPOINTS; i++){
    struct icc_endpoint *e = &icc_eps[i];

    acquire(&e->lock);
    if(e->count == 0 && icc_deadline_expired(e->deadline)){
      e->deadline = 0;
      wakeup(e);
    }
    release(&e->lock);
  }

  for(int i = 0; i < ICC_RPC_MAX_PENDING; i++){
    struct icc_rpc_pending *p = &icc_rpc_table[i];

    acquire(&p->lock);
    if(p->in_use && !p->has_reply && icc_deadline_expired(p->deadline)){
      p->deadline = 0;
      wakeup(p);
    }
    release(&p->lock);
  }
}
