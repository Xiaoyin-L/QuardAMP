#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "sbi.h"

void main();


// entry.S needs one stack per CPU.
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// entry.S jumps here in machine mode on stack0.
void
start()
{
  /* 关闭页表（使用裸地址模式），确保后续初始化代码在物理地址空间执行。
   * 这一步在 S-mode 下合法，satp 是 S-mode CSR。
   */
  w_satp(0);

  /* 开启 S-mode 中断使能：
   * SIE_SEIE = S-mode 外部中断（来自 PLIC，如 UART）
   * SIE_STIE = S-mode 定时器中断（来自 ACLINT，经 OpenSBI 委托）
   * 注意：不再设置 SIE_SSIE（软件中断），多核 IPI 后续通过 SBI HSM 处理
   */
  w_sie(r_sie() | SIE_SEIE | SIE_STIE);

  /* 设置第一次 timer 中断，让 OpenSBI 代写 mtimecmp。
   * 原版直接写 stimecmp，但 mtimecmp 是 M-mode 寄存器，S-mode 不可访问。
   * 通过 SBI TIME 扩展 ecall 请求 OpenSBI 设置。
   */
  sbi_set_timer(r_time() + 1000000);

  /* 进入主初始化 */
  main();

  for(;;);
}

