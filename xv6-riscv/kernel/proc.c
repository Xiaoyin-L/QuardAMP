#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

#define NICE_MIN   (-2)
#define NICE_MAX   2
#define NICE_0_WEIGHT 1024
#define VRUNTIME_SCALE 1024
#define MIN_GRANULARITY 2

// nice:   -2    -1    0     1    2
static int nice_to_weight[5] = {2048, 1536, 1024, 768, 512};

// 权重映射
static int
weight_from_nice(int nice)
{
  if(nice < NICE_MIN)
    nice = NICE_MIN;
  if(nice > NICE_MAX)
    nice = NICE_MAX;
  return nice_to_weight[nice - NICE_MIN];
}

// 用于给新进程找一个合理的初始 vruntime
// 寻找最小vruntime赋值给新进程
uint64 get_min_vruntime(void) 
{
  struct proc *p;
  uint64 min_vruntime = (uint64)-1;
  int found = 0;

  for(p =proc; p<&proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state==RUNNABLE || p->state==RUNNING) {
      if(!found || p->vruntime < min_vruntime) {
        found = 1;
        min_vruntime = p->vruntime;
      }
    }
    release(&p->lock);
  }

  if(found) return min_vruntime;
  return 0;
}


// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

  for(int i=0; i<NCPU; i++) {
    cpus[i].cfs.min_vruntime = 0;
    cpus[i].cfs.nr_running = 0;
  }

  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));

      p->vruntime = 0;
      p->exec_ticks = 0;
      p->nice = 0;
      p->weight = 0;
      p->home_cpu = 0;
      p->slice_ticks = 0;
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    p->vruntime = min_vruntime_cpu(p->home_cpu);
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // CFS初始化
  p->nice = 0;
  p->weight = weight_from_nice(p->nice);
  p->exec_ticks = 0;
  p->slice_ticks = 0;
  p->home_cpu = cpuid();
  p->last_migrate_tick = 0;
  


  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;

  // CFS字段
  p->nice = 0;
  p->exec_ticks = 0;
  p->weight = 0;
  p->vruntime = 0;
  p->home_cpu = 0;
  p->slice_ticks = 0;
  p->last_migrate_tick = 0;
}

// 辅助函数：统计某个 CPU 上的 runnable 数
int
count_runnable_on_cpu(int cpu_id)
{
  struct proc *p;
  int cnt = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == RUNNABLE && p->home_cpu == cpu_id) {
      cnt++;
    }
    release(&p->lock);    
  }
  return cnt;
}

// 辅助函数：寻找某个CPU上最小vruntime
uint64
min_vruntime_cpu(int cpu_id)
{
  struct proc *p;
  uint64 min = (uint64)-1;
  int found = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if((p->state == RUNNABLE || p->state == RUNNABLE) && (p->home_cpu == cpu_id)) {
      if(!found || p->vruntime < min) {
        min = p->vruntime;
        found = 1;
      }
    }
    release(&p->lock);
  }

  if(found)
    return min;
  return 0;
}

// 辅助函数：更新 CPU 本地 rq 统计
void
refresh_cpu_cfs_stats(int cpu_id)
{
  cpus[cpu_id].cfs.nr_running = count_runnable_on_cpu(cpu_id);
  cpus[cpu_id].cfs.min_vruntime = min_vruntime_cpu(cpu_id);
}

//辅助函数： 选本 CPU 上最小 vruntime 的进程
struct proc*
pick_next_proc_on_cpu(int cpu_id)
{
  struct proc *p;
  struct proc *best = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == RUNNABLE && p->home_cpu == cpu_id) {
      if(best == 0 || p->vruntime < best->vruntime) {
         if(best != 0)
          release(&best->lock);
        best = p;
        continue;
      }
    }
    release(&p->lock);
   }
  return best;
}

// 更新当前进程 `vruntime`
void
update_curr_vruntime(struct proc *p, uint64 delta_exec)
{
  uint64 delta_vruntime;

  if(p == 0 || p->weight <= 0)
    return;

  // vruntime += 实际运行时间 × (nice_0_weight / 当前进程权重)
  // 引入VRUNTIME_SCALE，目的是做定点放大，避免整数除法精度丢失
  delta_vruntime = delta_exec * NICE_0_WEIGHT * VRUNTIME_SCALE / p->weight;
  if(delta_vruntime == 0)
    delta_vruntime = 1;

  p->vruntime += delta_vruntime;
  p->exec_ticks += delta_exec;
}

// 辅助函数：判断当前进程是否应该被抢占
int
should_preempt_cfs(struct proc *curr)
{
  struct proc *p;
  int cpu_id;
  int preempt = 0;

  if(curr == 0)
    return 0;

  cpu_id = curr->home_cpu;

  for(p = proc; p < &proc[NPROC]; p++){
    if(p == curr) continue;
    acquire(&p->lock);
    if(p->state == RUNNABLE && p->home_cpu == cpu_id){
      if(p->vruntime < curr->vruntime){
        preempt = 1;
        release(&p->lock);
        break;
      }
    }
    release(&p->lock);
  }

  return preempt;
}

// 辅助函数
// 统计某 CPU 的 runnable 总权重
int
runnable_weight_on_cpu(int cpu_id)
{
  struct proc *p;
  int load = 0;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == RUNNABLE && p->home_cpu == cpu_id)
      load += p->weight;
    release(&p->lock);
  }
  return load;
}

// 多核辅助函数
// 找到除了当前 CPU 之外最忙碌的 CPU，用于负载均衡
int
find_busiest_cpu_by_load(void)
{
  int i;
  int busiest = -1;
  int max_load = -1;

  for(i = 0; i < NCPU; i++){
    int load = runnable_weight_on_cpu(i);
    if(load > max_load){
      max_load = load;
      busiest = i;
    }
  }

  if(max_load == 0)
    return -1;
  return busiest;
}

int
find_idlest_cpu_by_load(void)
{
  int i;
  int idlest = -1;
  int min_load = 0x7fffffff;

  for(i = 0; i < 3; i++){
    int load = runnable_weight_on_cpu(i);
    if(load < min_load){
      min_load = load;
      idlest = i;
    }
  }

  return idlest;
}

// 判断是否需要全局 rebalance
int
need_global_rebalance(int *src_cpu, int *dst_cpu)
{
  int busiest, idlest;
  int max_load, min_load;
  int max_nr, min_nr;

  busiest = find_busiest_cpu_by_load();
  idlest  = find_idlest_cpu_by_load();

  if(busiest < 0 || idlest < 0 || busiest == idlest)
    return 0;

  max_load = runnable_weight_on_cpu(busiest);
  min_load = runnable_weight_on_cpu(idlest);

  max_nr = count_runnable_on_cpu(busiest);
  min_nr = count_runnable_on_cpu(idlest);

  // 按 load 优先判断，其次参考 runnable 数量
  if(max_load - min_load >= 1024 ||
     max_nr   - min_nr   >= 2){
    *src_cpu = busiest;
    *dst_cpu = idlest;
    return 1;
  }

  return 0;
}

// 选迁移候选任务
struct proc*
pick_migration_candidate(int src_cpu)
{
  struct proc *p;
  struct proc *victim = 0;
  uint64 now_ticks = ticks;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);

    if(p->pid <= 3){
      release(&p->lock);
      continue;
    }

    if(p->state == RUNNABLE && p->home_cpu == src_cpu){
      if(now_ticks - p->last_migrate_tick < 20){
        release(&p->lock);
        continue;
      }

      if(victim == 0 || p->vruntime > victim->vruntime){
        if(victim != 0)
          release(&victim->lock);
        victim = p;
        continue;
      }
    }

    release(&p->lock);
  }

  return victim; // 返回时若非空，victim->lock 仍持有
}

// 迁移一个任务
struct proc*
migrate_one_task(int src_cpu, int dst_cpu)
{
  struct proc *p;
  uint64 dst_min;
  // uint64 old_vr;
  uint64 now_ticks = ticks;

  if(src_cpu < 0 || dst_cpu < 0 || src_cpu == dst_cpu)
    return 0;

  dst_min = min_vruntime_cpu(dst_cpu);

  p = pick_migration_candidate(src_cpu);
  if(p == 0)
    return 0;

  //old_vr = p->vruntime;
  

  p->home_cpu = dst_cpu;

  // 迁移后只允许抬高，不允许降低
  if(p->vruntime < dst_min)
    p->vruntime = dst_min;

  p->last_migrate_tick = now_ticks;

  // printf("migrate: pid=%d from cpu%d to cpu%d old_vr=%d new_vr=%d\n",
  //       p->pid, src_cpu, dst_cpu, (int)old_vr, (int)p->vruntime);

  release(&p->lock);
  return p;
}

//rebalance 函数
void
rebalance_cfs(void)
{
  int src_cpu, dst_cpu;

  if(need_global_rebalance(&src_cpu, &dst_cpu)){
    migrate_one_task(src_cpu, dst_cpu);
  }
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  acquire(&np->lock);
  np->nice = p->nice;
  np->exec_ticks = 0;
  np->weight = p->weight;
  np->vruntime = p->vruntime;
  np->slice_ticks = 0;
  np->home_cpu = np->pid % 2; // 简单的负载均衡：新进程先放到自己 PID 对应的 CPU 上
  release(&np->lock);

  printf("fork/alloc: pid=%d home_cpu=%d nice=%d vruntime=%d\n",
       np->pid, np->home_cpu, np->nice, (int)np->vruntime);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  int id = cpuid();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    refresh_cpu_cfs_stats(id);

    // 先选本 CPU 自己的任务
    p = pick_next_proc_on_cpu(id);

    // 本地没任务，则尝试从别的 CPU steal
    if(p == 0){
      rebalance_cfs();
      p = pick_next_proc_on_cpu(id);
    }

    if(p != 0) {
      p->state = RUNNING;
      p->slice_ticks = 0;
      c->proc = p;

      swtch(&c->context, &p->context);

      c->proc = 0;
      release(&p->lock);

    }
  }
}


// 设置进程优先级的封装函数
int
setnice(int pid, int nice)
{
  struct proc *p;

  if(nice < NICE_MIN || nice > NICE_MAX) {
    return -1;
  }

  // 遍历进程表
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->nice = nice;
      p->weight = weight_from_nice(nice);

      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// 获取进程nice值
int
getnice(int pid)
{
  struct proc *p;

  // 遍历进程表
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      int n = p->nice;
      release(&p->lock);
      return n;
    }
    release(&p->lock);
  }
  return -1;
}


// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
