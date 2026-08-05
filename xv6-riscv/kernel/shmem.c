#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"

#define SHMEM_EP_XV6_TEST  0x00000100
#define SHMEM_EP_RTOS_ECHO 0x00010100
#define SHMEM_CMD_TEST     0x00000200

struct shmem_ctrl {
  volatile uint32 magic;
  volatile uint32 version;
  volatile uint32 to_rtos_head;
  volatile uint32 to_rtos_tail;
  volatile uint32 to_xv6_head;
  volatile uint32 to_xv6_tail;
};

struct shmem_msg {
  uint32 src_ep;
  uint32 dst_ep;
  uint32 cmd;
  uint32 len;
  uint32 cookie;
  uint32 flags;
  char payload[40];
};

static struct spinlock shmem_lock;

static struct shmem_ctrl*
ctrl(void)
{
  return (struct shmem_ctrl*)(SHMEM_ADDR + SHMEM_CTRL_OFFSET);
}

static struct shmem_msg*
to_rtos_ring(void)
{
  return (struct shmem_msg*)(SHMEM_ADDR + SHMEM_TO_RTOS_OFFSET);
}

static struct shmem_msg*
to_xv6_ring(void)
{
  return (struct shmem_msg*)(SHMEM_ADDR + SHMEM_TO_XV6_OFFSET);
}

void
shmem_init(void)
{
  struct shmem_ctrl *c = ctrl();

  if(sizeof(struct shmem_msg) != SHMEM_MSG_SIZE)
    panic("shmem_msg size");

  initlock(&shmem_lock, "shmem");

  memset((void*)c, 0, SHMEM_CTRL_SIZE);
  memset((void*)to_rtos_ring(), 0, SHMEM_RING_BYTES);
  memset((void*)to_xv6_ring(), 0, SHMEM_RING_BYTES);

  c->version = SHMEM_VERSION;
  __sync_synchronize();
  c->magic = SHMEM_MAGIC;

  printf("shmem: addr=%p size=%d ctrl=%p to_rtos=%p to_xv6=%p\n",
         (void*)SHMEM_ADDR, SHMEM_SIZE, c, to_rtos_ring(), to_xv6_ring());
}

int
shmem_send_to_rtos(uint32 cookie)
{
  struct shmem_ctrl *c = ctrl();
  struct shmem_msg *ring = to_rtos_ring();
  struct shmem_msg *m;
  const char payload[] = "xv6->rtos phase4 hello";
  uint32 head;
  uint32 tail;
  uint32 idx;
  uint32 len;

  if(c->magic != SHMEM_MAGIC || c->version != SHMEM_VERSION){
    printf("shmem: not ready magic=%x version=%d\n", c->magic, c->version);
    return -1;
  }

  acquire(&shmem_lock);

  head = c->to_rtos_head;
  tail = c->to_rtos_tail;
  if(tail - head >= SHMEM_RING_SIZE){
    release(&shmem_lock);
    printf("shmem: to_rtos ring full head=%d tail=%d\n", head, tail);
    return -1;
  }

  idx = tail % SHMEM_RING_SIZE;
  m = &ring[idx];
  memset(m, 0, sizeof(*m));

  m->src_ep = SHMEM_EP_XV6_TEST;
  m->dst_ep = SHMEM_EP_RTOS_ECHO;
  m->cmd = SHMEM_CMD_TEST;
  len = strlen(payload);
  if(len > sizeof(m->payload))
    len = sizeof(m->payload);
  m->len = len;
  m->cookie = cookie;
  memmove(m->payload, payload, len);

  __sync_synchronize();
  c->to_rtos_tail = tail + 1;

  release(&shmem_lock);

  printf("shmem: send to_rtos (icc) idx=%d cookie=%x tail=%d\n",
         idx, cookie, tail + 1);
  mailbox_ring_to_rtos(SHMEM_DOORBELL_CH0);
  return 0;
}

void
shmem_consume_from_rtos(void)
{
  struct shmem_ctrl *c = ctrl();
  struct shmem_msg *ring = to_xv6_ring();
  struct shmem_msg *m;
  char payload[sizeof(m->payload) + 1];
  uint32 head;
  uint32 tail;
  uint32 idx;
  uint32 len;

  if(c->magic != SHMEM_MAGIC || c->version != SHMEM_VERSION){
    printf("shmem: drop to_xv6, not ready magic=%x version=%d\n",
           c->magic, c->version);
    return;
  }

  /*
   * 阶段 4 复用阶段 3 的反向消费路径：
   * 既能接收 FreeRTOS 的 vIccTestTask 主动消息，也能接收
   * ICC echo handler 对 xv6 请求的回复。
   */
  for(;;){
    head = c->to_xv6_head;
    tail = c->to_xv6_tail;
    if(head == tail)
      break;

    __sync_synchronize();

    idx = head % SHMEM_RING_SIZE;
    m = &ring[idx];
    len = m->len;
    if(len > sizeof(m->payload))
      len = sizeof(m->payload);
    memset(payload, 0, sizeof(payload));
    memmove(payload, m->payload, len);

    printf("shmem: recv from_rtos idx=%d src=%x dst=%x cmd=%x len=%d cookie=%x flags=%x payload=%s\n",
           idx, m->src_ep, m->dst_ep, m->cmd, m->len, m->cookie,
           m->flags, payload);

    __sync_synchronize();
    c->to_xv6_head = head + 1;
  }
}
