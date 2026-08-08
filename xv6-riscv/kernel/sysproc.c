#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "slab.h"
#include "shmem.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_setnice(void)
{
  int pid, nice;

  argint(0, &pid);
  argint(1, &nice);

  return setnice(pid, nice);
}

uint64
sys_getnice(void)
{
  int pid;

  argint(0, &pid);
  return getnice(pid);
}

uint64
sys_slabinfo(void)
{
  uint64 ubuf;
  int max_entries;
  struct slab_stat_entry kbuf[7];
  int count;
  struct proc *p;

  argaddr(0, &ubuf);
  argint(1, &max_entries);

  if(max_entries <= 0)
    return 0;
  if(max_entries > 7)
    max_entries = 7;

  count = slab_get_stats(kbuf, max_entries);
  p = myproc();
  if(copyout(p->pagetable, ubuf, (char*)kbuf,
             count * sizeof(struct slab_stat_entry)) < 0)
    return -1;

  return count;
}

// 阶段 1 单向 doorbell（xv6 -> FreeRTOS）测试入口：
// 用户态传入 reason，内核写 mailbox TX_TO_RTOS 寄存器。
// 设备锁存 reason、置 pending 并拉起 PLIC 源 13，
// FreeRTOS(hart7) 在其中断里打印收到。正式通道建立后该调用移除。
uint64
sys_mailboxring(void)
{
  int reason;

  argint(0, &reason);
  mailbox_ring_to_rtos((uint32)reason);
  return 0;
}

uint64
sys_shmemsend(void)
{
  int cookie;
  argint(0, &cookie);
  return shmem_send_to_rtos((uint32)cookie) == 0 ? 0 : (uint64)-1;
}

/*
 * Stage 5 ICC send syscall.
 *
 * User space supplies the remote endpoint, command, payload pointer/length and
 * cookie.  copyin() validates the user pointer before icc_send() publishes the
 * message into the shared-memory ring.
 */
uint64
sys_iccsend(void)
{
  int dst_ep;
  int cmd;
  int len;
  int cookie;
  uint64 payload_addr;
  char payload[SHMSG_PAYLOAD_SIZE];

  argint(0, &dst_ep);
  argint(1, &cmd);
  argaddr(2, &payload_addr);
  argint(3, &len);
  argint(4, &cookie);

  if(len < 0)
    len = 0;
  if(len > SHMSG_PAYLOAD_SIZE)
    len = SHMSG_PAYLOAD_SIZE;

  if(len > 0){
    if(copyin(myproc()->pagetable, payload, payload_addr, len) < 0)
      return -1;
  }

  return icc_send((uint32)dst_ep, (uint32)cmd, payload,
                  (uint32)len, (uint32)cookie) == 0 ? 0 : (uint64)-1;
}

/*
 * Stage 5 ICC receive syscall.
 *
 * The kernel blocks in icc_recv() until a message reaches the requested local
 * endpoint, then copies only the payload to user space and returns its length.
 */
uint64
sys_iccrecv(void)
{
  int ep;
  int max_len;
  int timeout_ms;
  uint64 buf_addr;

  argint(0, &ep);
  argaddr(1, &buf_addr);
  argint(2, &max_len);
  argint(3, &timeout_ms);

  if(max_len < 0)
    max_len = 0;
  if(timeout_ms < 0)
    timeout_ms = 0;

  return icc_recv_user((uint32)ep, myproc()->pagetable, buf_addr,
                       (uint32)max_len, (uint32)timeout_ms);
}

/*
 * Batched ICC send syscall.
 *
 * This keeps each message as a normal rpmsg buffer, but publishes a group of
 * descriptors before issuing one mailbox kick.  It gives the benchmark an
 * explicit way to measure virtqueue-style notification coalescing.
 */
uint64
sys_iccsendbatch(void)
{
  int dst_ep;
  int cmd;
  int len;
  int base_cookie;
  int count;
  uint64 payload_addr;
  char payload[SHMSG_PAYLOAD_SIZE];

  argint(0, &dst_ep);
  argint(1, &cmd);
  argaddr(2, &payload_addr);
  argint(3, &len);
  argint(4, &base_cookie);
  argint(5, &count);

  if(len < 0)
    len = 0;
  if(len > SHMSG_PAYLOAD_SIZE)
    len = SHMSG_PAYLOAD_SIZE;
  if(count < 0)
    count = 0;

  if(len > 0){
    if(copyin(myproc()->pagetable, payload, payload_addr, len) < 0)
      return -1;
  }

  return icc_send_batch((uint32)dst_ep, (uint32)cmd, payload,
                        (uint32)len, (uint32)base_cookie,
                        (uint32)count);
}

/*
 * Stage 6 RPC syscall.
 *
 * User space passes a remote endpoint, command, request payload and reply
 * buffer.  The kernel keeps cookie allocation private to icc_rpc_call(), so
 * callers cannot accidentally reuse a cookie and steal another process's
 * reply.  copyin/copyout remain the only places that touch user addresses.
 */
uint64
sys_rpccall(void)
{
  int dst_ep;
  int cmd;
  int plen;
  int reply_max;
  int timeout_ms;
  int copy_len;
  uint64 payload_addr;
  uint64 reply_addr;
  char payload[SHMSG_PAYLOAD_SIZE];
  char reply[SHMSG_PAYLOAD_SIZE];
  uint32 reply_len;
  struct proc *p;

  argint(0, &dst_ep);
  argint(1, &cmd);
  argaddr(2, &payload_addr);
  argint(3, &plen);
  argaddr(4, &reply_addr);
  argint(5, &reply_max);
  argint(6, &timeout_ms);

  if(plen < 0)
    plen = 0;
  if(plen > SHMSG_PAYLOAD_SIZE)
    plen = SHMSG_PAYLOAD_SIZE;
  if(reply_max < 0)
    reply_max = 0;
  if(reply_max > SHMSG_PAYLOAD_SIZE)
    reply_max = SHMSG_PAYLOAD_SIZE;
  if(timeout_ms < 0)
    timeout_ms = 0;

  p = myproc();
  if(plen > 0){
    if(copyin(p->pagetable, payload, payload_addr, plen) < 0)
      return -1;
  }

  memset(reply, 0, sizeof(reply));
  reply_len = 0;
  if(icc_rpc_call((uint32)dst_ep, (uint32)cmd, payload, (uint32)plen,
                  reply, (uint32)reply_max, &reply_len,
                  (uint32)timeout_ms) < 0)
    return -1;

  copy_len = reply_len;
  if(copy_len > reply_max)
    copy_len = reply_max;
  if(copy_len > SHMSG_PAYLOAD_SIZE)
    copy_len = SHMSG_PAYLOAD_SIZE;

  if(copy_len > 0){
    if(copyout(p->pagetable, reply_addr, reply, (uint64)copy_len) < 0)
      return -1;
  }

  return copy_len;
}

uint64
sys_rdtime(void)
{
  return r_time();
}

uint64
sys_rdcycle(void)
{
  return r_cycle();
}
