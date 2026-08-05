#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "sbi.h"

/* entry.S 中定义的入口标号，OpenSBI HSM 启动 secondary hart 时跳转到此地址。
 * 由于 xv6 内核采用恒等映射（VA == PA == 0x82000000），
 * 该符号的链接地址即为 OpenSBI 所需的物理入口地址。
 */
extern void _entry(void);

volatile static int started = 0;

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(cpuid() == 0){
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    kinit();         // physical page allocator
    slab_init();     // small object allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    shmem_init();
    icc_init();
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    virtio_disk_init(); // emulated hard disk
    userinit();      // first user process
    __sync_synchronize();
    started = 1;

    /* 多核启动：通过 SBI HSM 扩展逐一唤醒 hart1~hart6。
     *
     * 为什么不用原版 xv6 的方式？
     *   原版依赖 QEMU 在 M-mode 同时释放所有 hart，每个 hart 上电即跳入 _entry。
     *   但在 OpenSBI Domain 架构下，只有 boot hart（hart0）被自动启动，
     *   其余 hart 处于 HSM STOPPED 状态，必须显式调用 sbi_hart_start() 唤醒。
     *
     * 参数说明：
     *   hartid     - 目标 hart 编号（1~6，hart7 属于 trusted-domain 不可启动）
     *   _entry     - 目标 hart 开始执行的 S-mode 入口地址
     *   opaque (0) - 传递给目标 hart 的 a1 参数，此处无需额外信息
     *
     * 被唤醒的 hart 执行路径：
     *   _entry → start() → main()（cpuid()!=0 分支）→ per-hart init → scheduler()
     */
    for(int hart = 1; hart < NCPU - 1; hart++){
      int ret = sbi_hart_start(hart, (uint64)_entry, 0);
      if(ret != 0){
        printf("hart %d start failed: error %d\n", hart, ret);
      }
    }
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }

  scheduler();        
}
