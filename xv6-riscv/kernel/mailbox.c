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


// 阶段 2：FreeRTOS -> xv6 方向 doorbell 中断处理。
// 由 trap.c devintr() 在 plic_claim() 返回源 14 后调用。
//
// 处理顺序与阶段 1 FreeRTOS 侧同构：
//   读 reason -> W1C ack -> （返回 devintr 后）plic_complete。
// 若 ack 滞后于 plic_complete，设备侧中断条件仍在，
// PLIC complete 后会立即再次 pending，造成中断风暴。
//
void
mailboxintr(void)
{
  // 只读不清：pending 位保持到显式 ack。
  uint32 reason = *(volatile uint32*)MAILBOX_RX_FROM_RTOS;

  // W1C：向 RX_FROM_RTOS 写 bit1（MAILBOX_TO_XV6_BIT=0x2）清 to_xv6 pending，
  // 设备撤销 PLIC 源 14 的 IRQ 线。写 0 无效，避免误清另一方向。
  *(volatile uint32*)MAILBOX_RX_FROM_RTOS = MAILBOX_TO_XV6_BIT;

  // 阶段 2 验证标准：FreeRTOS 触发后 xv6 kernel 打印收到中断。
  printf("mailbox irq from FreeRTOS, reason=%x\n", reason);
}