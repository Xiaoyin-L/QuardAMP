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

// RISC-V AIA interrupt controllers.
// External device lines enter the S-level APLIC and are delivered as
// MSIs to each hart's S-level IMSIC.
#define APLIC_S 0x0d000000L
#define APLIC_DOMAINCFG      (APLIC_S + 0x0000)
#define APLIC_SOURCECFG(irq) (APLIC_S + 0x0004 + ((irq) - 1) * 4)
#define APLIC_SETIENUM       (APLIC_S + 0x1edc)
#define APLIC_TARGET(irq)    (APLIC_S + 0x3004 + ((irq) - 1) * 4)
#define APLIC_DOMAINCFG_IE   (1 << 8)
#define APLIC_DOMAINCFG_DM   (1 << 2)
#define APLIC_SOURCECFG_SM_LEVEL_HIGH 0x6
#define APLIC_TARGET_HART_IDX_SHIFT 18

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

#define QUARD_IOMMU 0x10005000L
#define QUARD_IOMMU_SIZE 0x1000

#define PCIE_ECAM 0x30000000L
#define PCIE_ECAM_SIZE 0x100000L
#define PCIE_MMIO 0x40000000L
#define PCIE_MMIO_SIZE 0x10000L
#define PCIE_ACCEL_BAR0 PCIE_MMIO
#define PCIE_ACCEL_MSI_IRQ 32

// STATUS / ack 位定义（与 QEMU hw/misc/quardamp_mailbox.c 一致）。
// ack 采用 W1C：向 RX 寄存器写对应位 1 清 pending。
#define MAILBOX_TO_RTOS_BIT (1 << 0)
#define MAILBOX_TO_XV6_BIT  (1 << 1)

// AMP shared memory layout.
#define SHMEM_ADDR 0xb2000000L
#define SHMEM_SIZE (1024 * 1024)
#define SHMEM_CTRL_OFFSET    0x0000
#define SHMEM_VRING_TO_RTOS_OFFSET 0x1000
#define SHMEM_VRING_TO_XV6_OFFSET  0x3000
#define SHMEM_BUF_TO_RTOS_OFFSET   0x5000
#define SHMEM_BUF_TO_XV6_OFFSET    0xd000
#define SHMEM_CTRL_SIZE      0x1000
#define SHMEM_RING_SIZE 64
#define SHMEM_VRING_BYTES 0x2000
#define SHMEM_RPMSG_BUF_SIZE 512
#define SHMEM_RPMSG_BUF_BYTES (SHMEM_RING_SIZE * SHMEM_RPMSG_BUF_SIZE)
#define SHMEM_MAGIC 0x51414d50
#define SHMEM_VERSION 2
#define SHMEM_DOORBELL_VRING_TO_RTOS (1 << 0)
#define SHMEM_DOORBELL_VRING_TO_XV6  (1 << 1)
#define SHMEM_DOORBELL_CH0 SHMEM_DOORBELL_VRING_TO_RTOS

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x82000000 to PHYSTOP.
/* KERNBASE 必须与 kernel.ld 的链接地址一致，
 * 因为 xv6 的虚拟地址直接映射物理地址（恒等映射），
 * 如果 KERNBASE 和实际加载地址不一致，页表映射会指向错误的物理页 
 */
#define KERNBASE 0x82000000L
#define RAMTOP (KERNBASE + 128*1024*1024)

// Stage 2 PCIe DMA carveout.
// The normal page allocator stops at PHYSTOP, so this top-of-RAM region is
// reserved for device-visible bounce buffers managed by kernel/dma.c.
#define DMA_CARVEOUT_SIZE (1024 * 1024)
#define DMA_CARVEOUT_BASE (RAMTOP - DMA_CARVEOUT_SIZE)
#define DMA_CARVEOUT_END  RAMTOP
#define DMA_CACHELINE_SIZE 64

#define PHYSTOP DMA_CARVEOUT_BASE

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
