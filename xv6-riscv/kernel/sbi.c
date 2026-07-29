#include "types.h"
#include "sbi.h"

/* SBI 返回值结构：error 为 0 表示成功，value 为扩展定义的返回值 */
struct sbiret {
  long error;
  long value;
};

/* SBI ecall 通用封装
 * RISC-V SBI 调用约定：
 *   a7 = extension ID（标识哪个 SBI 扩展）
 *   a6 = function ID（标识扩展内的具体功能）
 *   a0~a5 = 参数
 *   返回值：a0 = error code, a1 = value
 *
 * 执行 ecall 后 CPU 硬件自动从 S-mode 陷入 M-mode，
 * 由 OpenSBI 的 trap handler 分发处理
 */
static inline struct sbiret
sbi_ecall(long ext, long fid, uint64 arg0, uint64 arg1, uint64 arg2,
          uint64 arg3, uint64 arg4, uint64 arg5)
{
  register uint64 a0 asm("a0") = arg0;
  register uint64 a1 asm("a1") = arg1;
  register uint64 a2 asm("a2") = arg2;
  register uint64 a3 asm("a3") = arg3;
  register uint64 a4 asm("a4") = arg4;
  register uint64 a5 asm("a5") = arg5;
  register uint64 a6 asm("a6") = fid;
  register uint64 a7 asm("a7") = ext;

  asm volatile("ecall"
               : "+r"(a0), "+r"(a1)
               : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
               : "memory");

  return (struct sbiret){ .error = a0, .value = a1 };
}

/* SBI TIME 扩展 (extension ID = 0x54494d45 即 ASCII "TIME")
 * function 0 = set_timer
 * 效果：OpenSBI 写 ACLINT 的 mtimecmp 寄存器，
 *       当 mtime >= stime_value 时硬件产生 timer 中断，
 *       经 OpenSBI 委托后以 S-mode timer interrupt 形式送达 xv6
 */
void
sbi_set_timer(uint64 stime_value)
{
  sbi_ecall(0x54494d45, 0, stime_value, 0, 0, 0, 0, 0);
}

/* SBI HSM 扩展 (extension ID = 0x48534d 即 ASCII "HSM")
 * function 0 = hart_start
 * 效果：OpenSBI 唤醒指定 hart，使其从 start_addr 以 S-mode 开始执行，
 *       a0 = hartid, a1 = opaque
 * 注意：不能启动 hart7，它属于 trusted-domain
 */
int
sbi_hart_start(uint64 hartid, uint64 start_addr, uint64 opaque)
{
  struct sbiret ret = sbi_ecall(0x48534d, 0, hartid, start_addr, opaque, 0, 0, 0);
  return ret.error;
}