#pragma once
#include "types.h"

/* SBI TIME 扩展：设置下一次 timer 中断的触发时间
 * 参数 stime_value：mtime 比较值，当 mtime >= stime_value 时产生 S-mode timer 中断
 * 替代原版的 w_stimecmp()，因为 mtimecmp 寄存器只有 M-mode 可写
 */
void sbi_set_timer(uint64 stime_value);

/* SBI HSM 扩展：启动指定的 hart
 * 参数 hartid：目标 hart 编号
 * 参数 start_addr：该 hart 开始执行的地址（S-mode）
 * 参数 opaque：传递给目标 hart 的 a1 参数
 * 用于多核启动阶段，由 boot hart 调用以唤醒 hart1~hart6
 */
int sbi_hart_start(uint64 hartid, uint64 start_addr, uint64 opaque);
