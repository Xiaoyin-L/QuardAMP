// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0 
// 10001000 -- virtio disk 
// 80000000 -- qemu's boot ROM loads the kernel here,
//             then jumps here.
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio interface
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// AMP mailbox (quardamp-mailbox) doorbell device.
// 地址与 PLIC 源编号必须与 QEMU hw/riscv/quard_star.c、
// dts/quard_star_sbi.dts 及 FreeRTOS 侧 driver/quard_star.h 手工对齐。
// 寄存器语义：TX 写触发 / RX 读不清 / W1C ack（见阶段 0 设计文档）。
#define MAILBOX 0x10004000L
#define MAILBOX_TX_TO_RTOS   (MAILBOX + 0x00)  // xv6 -> FreeRTOS doorbell
#define MAILBOX_TX_TO_XV6    (MAILBOX + 0x04)  // FreeRTOS -> xv6 doorbell (阶段 2)
#define MAILBOX_RX_FROM_XV6  (MAILBOX + 0x08)  // FreeRTOS 读 reason / W1C ack
#define MAILBOX_RX_FROM_RTOS (MAILBOX + 0x0c)  // xv6 读 reason / W1C ack (阶段 2)
#define MAILBOX_STATUS       (MAILBOX + 0x10)
#define MAILBOX_IRQ_MASK     (MAILBOX + 0x14)
#define MAILBOX_TO_RTOS_IRQ 13
#define MAILBOX_TO_XV6_IRQ  14

// STATUS / ack 位定义（与 QEMU hw/misc/quardamp_mailbox.c 一致）。
// ack 采用 W1C：向 RX 寄存器写对应位 1 清 pending。
#define MAILBOX_TO_RTOS_BIT (1 << 0)
#define MAILBOX_TO_XV6_BIT  (1 << 1)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x82000000 to PHYSTOP.
/* KERNBASE 必须与 kernel.ld 的链接地址一致，
 * 因为 xv6 的虚拟地址直接映射物理地址（恒等映射），
 * 如果 KERNBASE 和实际加载地址不一致，页表映射会指向错误的物理页 
 */
#define KERNBASE 0x82000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
