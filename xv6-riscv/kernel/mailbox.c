#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

//
// AMP mailbox (quardamp-mailbox) doorbell —— xv6 侧。
//
// 阶段 1 只做单向 doorbell：xv6 -> FreeRTOS。
// 写 TX_TO_RTOS 寄存器 = 按一次 doorbell：
//   设备锁存 reason 到 RX_FROM_XV6，置 to_rtos pending，
//   拉起 PLIC 源 13，由 FreeRTOS(hart7) 在其 S-mode context 接收。
// reason 低 16 位预留给上层编码"哪条 vring/endpoint 有数据"，
// 阶段 1 仅作冒烟标识。
//
// 阶段 2 将在此文件补 FreeRTOS -> xv6 方向的接收/ack。
//

void
mailbox_ring_to_rtos(uint32 reason)
{
  // volatile：doorbell 是有副作用的 MMIO 写，
  // 必须保证真正写到设备，不能被编译器优化或合并。
  // 设备要求 32 位对齐原子写（QEMU 侧限定 4 字节访问）。
  *(volatile uint32*)MAILBOX_TX_TO_RTOS = reason;
}