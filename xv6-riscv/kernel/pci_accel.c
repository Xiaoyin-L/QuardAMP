#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "dma.h"
#include "iommu.h"
#include "pcie_accel.h"

#define QACC_VENDOR_ID 0x1efd
#define QACC_DEVICE_ID 0xa001

#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_STATUS         0x06
#define PCI_BAR0           0x10
#define PCI_BAR4           0x20
#define PCI_CAP_PTR        0x34
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_MASTER 0x0004
#define PCI_STATUS_CAPLIST 0x0010
#define PCI_CAP_ID_MSI     0x05
#define PCI_CAP_ID_MSIX    0x11

#define QACC_REG_MAGIC      0x00
#define QACC_REG_STATUS     0x04
#define QACC_REG_SRC_LO     0x08
#define QACC_REG_SRC_HI     0x0c
#define QACC_REG_DST_LO     0x10
#define QACC_REG_DST_HI     0x14
#define QACC_REG_LEN        0x18
#define QACC_REG_CMD        0x1c
#define QACC_REG_IRQ_STATUS 0x20
#define QACC_REG_IRQ_ACK    0x24
#define QACC_REG_IOMMU_CTRL 0x28
#define QACC_REG_IOMMU_STATUS 0x2c
#define QACC_REG_IOVA_LO    0x30
#define QACC_REG_IOVA_HI    0x34
#define QACC_REG_PA_LO      0x38
#define QACC_REG_PA_HI      0x3c
#define QACC_REG_MAP_LEN    0x40
#define QACC_REG_MAP_PERM   0x44
#define QACC_REG_FAULT_LO   0x48
#define QACC_REG_FAULT_HI   0x4c
#define QACC_REG_QUEUE_SEL  0x50
#define QACC_REG_QUEUE_DOMAIN 0x54
#define QACC_REG_QUEUE_VECTOR 0x58
#define QACC_REG_QUEUE_COUNT 0x5c

#define QACC_MAGIC          0x51414343U
#define QACC_STATUS_DONE    0x1U
#define QACC_STATUS_FAULT   0x4U
#define QACC_CMD_RUN        0x1U
#define QACC_CMD_IRQ        0x2U
#define QACC_IRQ_DONE       0x1U
#define QACC_TEST_LEN       128
#define QACC_BENCH_BUF_SIZE 4096
#define QACC_IOMMU_ENABLE   0x1U
#define QACC_IOMMU_FAULT    0x1U
#define QACC_MAP_READ       0x1U
#define QACC_MAP_WRITE      0x2U
#define QACC_IOVA_BASE      0x10000000UL
#define QACC_IOVA_SIZE      0x4000UL
#define QACC_QUEUE0         0
#define QACC_QUEUE1         1
#define QACC_DOMAIN_XV6     0
#define QACC_DOMAIN_RTOS    1
#define QACC_VECTOR0        0
#define QACC_VECTOR1        1
#define QACC_MSIX_TABLE_ENTRY_SIZE 16

struct qacc_state {
  int present;
  int bus;
  int dev;
  int func;
  uint64 bar0;
  uint64 msix_bar;
  volatile int irq_seen[2];
  uint32 irq_count;
  uint32 vector_count[2];
  struct spinlock lock;
  struct dma_buf src_dma;
  struct dma_buf dst_dma;
  uint64 src_iova;
  uint64 dst_iova;
  uint64 iova_base;
  uint64 iova_size;
};

static struct qacc_state qacc;

static volatile uint32*
pci_cfg_addr(int bus, int dev, int func, int off)
{
  uint64 addr = PCIE_ECAM +
    ((uint64)bus << 20) + ((uint64)dev << 15) +
    ((uint64)func << 12) + (off & ~3);
  return (volatile uint32*)addr;
}

static uint32
pci_cfg_read32(int bus, int dev, int func, int off)
{
  return *pci_cfg_addr(bus, dev, func, off);
}

static void
pci_cfg_write32(int bus, int dev, int func, int off, uint32 val)
{
  *pci_cfg_addr(bus, dev, func, off) = val;
}

static uint16
pci_cfg_read16(int bus, int dev, int func, int off)
{
  uint32 v = pci_cfg_read32(bus, dev, func, off);
  return (uint16)(v >> ((off & 2) * 8));
}

static void
pci_cfg_write16(int bus, int dev, int func, int off, uint16 val)
{
  volatile uint32 *p = pci_cfg_addr(bus, dev, func, off);
  int shift = (off & 2) * 8;
  uint32 v = *p;

  v &= ~(0xffffU << shift);
  v |= (uint32)val << shift;
  *p = v;
}

static volatile uint32*
qacc_reg(uint32 off)
{
  return (volatile uint32*)(qacc.bar0 + off);
}

static int
pci_find_qacc(void)
{
  for(int dev = 0; dev < 32; dev++){
    uint16 vendor = pci_cfg_read16(0, dev, 0, PCI_VENDOR_ID);
    uint16 device = pci_cfg_read16(0, dev, 0, PCI_DEVICE_ID);

    if(vendor == QACC_VENDOR_ID && device == QACC_DEVICE_ID){
      qacc.bus = 0;
      qacc.dev = dev;
      qacc.func = 0;
      return 0;
    }
  }
  return -1;
}

static int
pci_find_cap(uint8 id)
{
  uint16 status = pci_cfg_read16(qacc.bus, qacc.dev, qacc.func, PCI_STATUS);

  if((status & PCI_STATUS_CAPLIST) == 0)
    return 0;

  uint8 cap = pci_cfg_read32(qacc.bus, qacc.dev, qacc.func, PCI_CAP_PTR) & 0xff;
  for(int i = 0; cap && i < 48; i++){
    uint32 hdr = pci_cfg_read32(qacc.bus, qacc.dev, qacc.func, cap);
    if((hdr & 0xff) == id)
      return cap;
    cap = (hdr >> 8) & 0xff;
  }
  return 0;
}

static void
pci_program_bars(void)
{
  uint32 cmd = pci_cfg_read16(qacc.bus, qacc.dev, qacc.func, PCI_COMMAND);

  pci_cfg_write16(qacc.bus, qacc.dev, qacc.func, PCI_COMMAND,
                  cmd & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));
  pci_cfg_write32(qacc.bus, qacc.dev, qacc.func, PCI_BAR0,
                  (uint32)PCIE_ACCEL_BAR0);
  pci_cfg_write32(qacc.bus, qacc.dev, qacc.func, PCI_BAR4,
                  (uint32)PCIE_ACCEL_MSIX_BAR);
  pci_cfg_write16(qacc.bus, qacc.dev, qacc.func, PCI_COMMAND,
                  cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
  qacc.bar0 = PCIE_ACCEL_BAR0;
  qacc.msix_bar = PCIE_ACCEL_MSIX_BAR;
}

static int
pci_program_msi(void)
{
  int cap = pci_find_cap(PCI_CAP_ID_MSI);
  uint16 ctrl;
  uint64 msg_addr = 0x28000000UL;

  if(cap == 0)
    return -1;

  ctrl = pci_cfg_read16(qacc.bus, qacc.dev, qacc.func, cap + 2);
  pci_cfg_write32(qacc.bus, qacc.dev, qacc.func, cap + 4, (uint32)msg_addr);
  pci_cfg_write32(qacc.bus, qacc.dev, qacc.func, cap + 8, (uint32)(msg_addr >> 32));
  pci_cfg_write16(qacc.bus, qacc.dev, qacc.func, cap + 12, PCIE_ACCEL_MSI_IRQ);
  pci_cfg_write16(qacc.bus, qacc.dev, qacc.func, cap + 2, ctrl | 0x1);
  return 0;
}

static volatile uint32*
qacc_msix_table_reg(uint32 vector, uint32 off)
{
  return (volatile uint32*)(qacc.msix_bar +
                            vector * QACC_MSIX_TABLE_ENTRY_SIZE + off);
}

static void
qacc_program_msix_vector(uint32 vector, uint32 irq)
{
  uint64 msg_addr = 0x28000000UL;

  *qacc_msix_table_reg(vector, 12) = 1; // mask while programming
  *qacc_msix_table_reg(vector, 0) = (uint32)msg_addr;
  *qacc_msix_table_reg(vector, 4) = (uint32)(msg_addr >> 32);
  *qacc_msix_table_reg(vector, 8) = irq;
  __sync_synchronize();
  *qacc_msix_table_reg(vector, 12) = 0;
}

static int
pci_program_msix(void)
{
  int cap = pci_find_cap(PCI_CAP_ID_MSIX);
  uint16 ctrl;
  uint32 table;

  if(cap == 0)
    return -1;

  table = pci_cfg_read32(qacc.bus, qacc.dev, qacc.func, cap + 4);
  if((table & 0x7) != 4 || (table & ~0x7) != 0)
    return -1;

  qacc_program_msix_vector(QACC_VECTOR0, PCIE_ACCEL_MSIX_IRQ0);
  qacc_program_msix_vector(QACC_VECTOR1, PCIE_ACCEL_MSIX_IRQ1);

  ctrl = pci_cfg_read16(qacc.bus, qacc.dev, qacc.func, cap + 2);
  ctrl &= ~(1 << 14); // function mask off
  ctrl |= (1 << 15);  // MSI-X enable
  pci_cfg_write16(qacc.bus, qacc.dev, qacc.func, cap + 2, ctrl);
  return 0;
}

static void
qacc_select_queue(uint32 queue)
{
  *qacc_reg(QACC_REG_QUEUE_SEL) = queue;
  __sync_synchronize();
}

static void
qacc_program_queue(uint32 queue, uint32 domain, uint32 vector)
{
  qacc_select_queue(queue);
  *qacc_reg(QACC_REG_QUEUE_DOMAIN) = domain;
  *qacc_reg(QACC_REG_QUEUE_VECTOR) = vector;
  __sync_synchronize();
}

static void
qacc_program_iommu(void)
{
  qacc.iova_base = QACC_IOVA_BASE;
  qacc.iova_size = QACC_IOVA_SIZE;
  qacc.src_iova = qacc.iova_base;
  qacc.dst_iova = qacc.iova_base + (qacc.dst_dma.pa - qacc.src_dma.pa);

  iommu_disable();
  iommu_map_domain_window(QACC_DOMAIN_XV6, qacc.iova_base, qacc.src_dma.pa,
                          qacc.iova_size,
                          IOMMU_PERM_READ | IOMMU_PERM_WRITE);
  iommu_clear_domain_fault(QACC_DOMAIN_XV6);
  iommu_clear_domain_fault(QACC_DOMAIN_RTOS);
  iommu_enable();
}

void
pcie_accel_init(void)
{
  initlock(&qacc.lock, "pcieaccel");

  if(pci_find_qacc() < 0){
    printf("pcie-accel: device not found\n");
    return;
  }

  pci_program_bars();
  if(pci_program_msix() < 0 && pci_program_msi() < 0){
    printf("pcie-accel: MSI/MSI-X capability not found\n");
    return;
  }

  if(*qacc_reg(QACC_REG_MAGIC) != QACC_MAGIC){
    printf("pcie-accel: bad BAR0 magic %x\n", *qacc_reg(QACC_REG_MAGIC));
    return;
  }

  if(dma_alloc(&qacc.src_dma, QACC_BENCH_BUF_SIZE, DMA_CACHELINE_SIZE) < 0 ||
     dma_alloc(&qacc.dst_dma, QACC_BENCH_BUF_SIZE, DMA_CACHELINE_SIZE) < 0){
    printf("pcie-accel: DMA buffer allocation failed\n");
    return;
  }

  if(qacc.dst_dma.pa < qacc.src_dma.pa ||
     qacc.dst_dma.pa + qacc.dst_dma.size > qacc.src_dma.pa + QACC_IOVA_SIZE){
    printf("pcie-accel: DMA buffers are not inside one IOVA window\n");
    return;
  }
  qacc_program_iommu();
  qacc_program_queue(QACC_QUEUE0, QACC_DOMAIN_XV6, QACC_VECTOR0);
  qacc_program_queue(QACC_QUEUE1, QACC_DOMAIN_RTOS, QACC_VECTOR1);
  qacc_select_queue(QACC_QUEUE0);

  qacc.present = 1;
  printf("pcie-accel: bdf=%d:%d.%d bar0=0x%lx msix_bar=0x%lx vectors=%d/%d queues=%d domains=2 dma_src=0x%lx dma_dst=0x%lx iova=[0x%lx,0x%lx)\n",
         qacc.bus, qacc.dev, qacc.func, qacc.bar0, qacc.msix_bar,
         PCIE_ACCEL_MSIX_IRQ0, PCIE_ACCEL_MSIX_IRQ1,
         *qacc_reg(QACC_REG_QUEUE_COUNT),
         qacc.src_dma.pa, qacc.dst_dma.pa,
         qacc.iova_base, qacc.iova_base + qacc.iova_size);
}

void
pcie_accel_intr(int irq)
{
  int vector = irq - PCIE_ACCEL_MSIX_IRQ0;
  uint32 pending;

  if(!qacc.present)
    return;

  if(vector < 0 || vector > 1)
    vector = 0;

  pending = *qacc_reg(QACC_REG_IRQ_STATUS);
  if(pending){
    *qacc_reg(QACC_REG_IRQ_ACK) = pending;
    acquire(&qacc.lock);
    qacc.irq_seen[vector] = 1;
    qacc.irq_count++;
    qacc.vector_count[vector]++;
    release(&qacc.lock);
  }
}

static int
qacc_wait_irq(int vector, uint timeout_ticks)
{
  uint ticks0;

  acquire(&tickslock);
  ticks0 = ticks;
  release(&tickslock);

  for(;;){
    int seen;

    acquire(&qacc.lock);
    seen = qacc.irq_seen[vector];
    release(&qacc.lock);
    if(seen)
      return 0;

    acquire(&tickslock);
    if(ticks - ticks0 > timeout_ticks){
      release(&tickslock);
      return -1;
    }
    release(&tickslock);
  }
}

static void
qacc_reset_irq_seen(int vector)
{
  acquire(&qacc.lock);
  qacc.irq_seen[vector] = 0;
  release(&qacc.lock);
}

static uint64
qacc_fault_addr(uint32 domain)
{
  return iommu_domain_fault_addr(domain);
}

static void
qacc_fill_src(uint32 len)
{
  uchar *p = (uchar*)qacc.src_dma.va;

  for(uint32 i = 0; i < len; i++)
    p[i] = (uchar)i;
}

static int
qacc_run_dma(uint32 queue, uint32 vector, uint64 src_iova, uint64 dst_iova,
             uint32 len, int expect_fault)
{
  uint32 status;

  qacc_select_queue(queue);
  *qacc_reg(QACC_REG_IRQ_ACK) = 1U << queue;
  *qacc_reg(QACC_REG_STATUS) = QACC_STATUS_DONE | QACC_STATUS_FAULT;
  qacc_reset_irq_seen(vector);

  __sync_synchronize();
  *qacc_reg(QACC_REG_SRC_LO) = (uint32)src_iova;
  *qacc_reg(QACC_REG_SRC_HI) = (uint32)(src_iova >> 32);
  *qacc_reg(QACC_REG_DST_LO) = (uint32)dst_iova;
  *qacc_reg(QACC_REG_DST_HI) = (uint32)(dst_iova >> 32);
  *qacc_reg(QACC_REG_LEN) = len;
  *qacc_reg(QACC_REG_CMD) = QACC_CMD_RUN | QACC_CMD_IRQ;
  __sync_synchronize();

  if(qacc_wait_irq(vector, 20) < 0)
    return -1;

  status = *qacc_reg(QACC_REG_STATUS);
  if(expect_fault)
    return (status & QACC_STATUS_FAULT) ? 0 : -1;
  return (status & QACC_STATUS_FAULT) ? -1 : 0;
}

static int
qacc_verify_dst(uint32 len)
{
  uchar *src = (uchar*)qacc.src_dma.va;
  uchar *dst = (uchar*)qacc.dst_dma.va;

  for(uint32 i = 0; i < len; i++){
    uchar expected = src[i] ^ 0x5a;
    if(dst[i] != expected)
      return -1;
  }
  return 0;
}

int
pcie_accel_selftest(void)
{
  uchar cpu_src[QACC_TEST_LEN];
  uchar cpu_dst[QACC_TEST_LEN];
  uint64 bad_iova;

  if(!qacc.present)
    return -1;

  for(int i = 0; i < QACC_TEST_LEN; i++){
    cpu_src[i] = (uchar)i;
    cpu_dst[i] = 0;
  }

  if(dma_bounce_to_device(&qacc.src_dma, cpu_src, QACC_TEST_LEN) < 0)
    return -1;
  memset(qacc.dst_dma.va, 0, qacc.dst_dma.size);
  dma_sync_for_device(&qacc.dst_dma);

  qacc_select_queue(QACC_QUEUE0);
  iommu_clear_domain_fault(QACC_DOMAIN_XV6);
  *qacc_reg(QACC_REG_STATUS) = QACC_STATUS_DONE | QACC_STATUS_FAULT;
  qacc_reset_irq_seen(QACC_VECTOR0);

  __sync_synchronize();
  *qacc_reg(QACC_REG_SRC_LO) = (uint32)qacc.src_iova;
  *qacc_reg(QACC_REG_SRC_HI) = (uint32)(qacc.src_iova >> 32);
  *qacc_reg(QACC_REG_DST_LO) = (uint32)qacc.dst_iova;
  *qacc_reg(QACC_REG_DST_HI) = (uint32)(qacc.dst_iova >> 32);
  *qacc_reg(QACC_REG_LEN) = QACC_TEST_LEN;
  *qacc_reg(QACC_REG_CMD) = QACC_CMD_RUN | QACC_CMD_IRQ;
  __sync_synchronize();

  if(qacc_wait_irq(QACC_VECTOR0, 20) < 0){
    printf("pcie-accel: timeout status=%x irq_status=%x\n",
           *qacc_reg(QACC_REG_STATUS), *qacc_reg(QACC_REG_IRQ_STATUS));
    return -1;
  }
  if((*qacc_reg(QACC_REG_STATUS) & QACC_STATUS_FAULT) != 0 ||
     (iommu_domain_status(QACC_DOMAIN_XV6) & QACC_IOMMU_FAULT) != 0){
    printf("pcie-accel: unexpected IOMMU fault addr=0x%lx status=%x\n",
           qacc_fault_addr(QACC_DOMAIN_XV6),
           iommu_domain_status(QACC_DOMAIN_XV6));
    return -1;
  }

  if(dma_bounce_from_device(cpu_dst, &qacc.dst_dma, QACC_TEST_LEN) < 0)
    return -1;

  for(int i = 0; i < QACC_TEST_LEN; i++){
    uchar expected = cpu_src[i] ^ 0x5a;
    if(cpu_dst[i] != expected){
      printf("pcie-accel: mismatch byte %d got=%x expected=%x\n",
             i, cpu_dst[i], expected);
      return -1;
    }
  }

  bad_iova = qacc.iova_base + qacc.iova_size + DMA_CACHELINE_SIZE;
  *qacc_reg(QACC_REG_IRQ_ACK) = QACC_IRQ_DONE;
  iommu_clear_domain_fault(QACC_DOMAIN_XV6);
  *qacc_reg(QACC_REG_STATUS) = QACC_STATUS_DONE | QACC_STATUS_FAULT;
  qacc_reset_irq_seen(QACC_VECTOR0);
  __sync_synchronize();
  *qacc_reg(QACC_REG_SRC_LO) = (uint32)bad_iova;
  *qacc_reg(QACC_REG_SRC_HI) = (uint32)(bad_iova >> 32);
  *qacc_reg(QACC_REG_DST_LO) = (uint32)qacc.dst_iova;
  *qacc_reg(QACC_REG_DST_HI) = (uint32)(qacc.dst_iova >> 32);
  *qacc_reg(QACC_REG_LEN) = QACC_TEST_LEN;
  *qacc_reg(QACC_REG_CMD) = QACC_CMD_RUN | QACC_CMD_IRQ;
  __sync_synchronize();

  if(qacc_wait_irq(QACC_VECTOR0, 20) < 0){
    printf("pcie-accel: IOMMU fault test timeout status=%x irq_status=%x\n",
           *qacc_reg(QACC_REG_STATUS), *qacc_reg(QACC_REG_IRQ_STATUS));
    return -1;
  }
  if((*qacc_reg(QACC_REG_STATUS) & QACC_STATUS_FAULT) == 0 ||
     (iommu_domain_status(QACC_DOMAIN_XV6) & QACC_IOMMU_FAULT) == 0 ||
     qacc_fault_addr(QACC_DOMAIN_XV6) != bad_iova){
    printf("pcie-accel: IOMMU failed to block bad_iova=0x%lx fault=0x%lx status=%x iommu=%x\n",
           bad_iova, qacc_fault_addr(QACC_DOMAIN_XV6),
           *qacc_reg(QACC_REG_STATUS),
           iommu_domain_status(QACC_DOMAIN_XV6));
    return -1;
  }
  *qacc_reg(QACC_REG_IRQ_ACK) = QACC_IRQ_DONE;
  iommu_clear_domain_fault(QACC_DOMAIN_XV6);
  *qacc_reg(QACC_REG_STATUS) = QACC_STATUS_DONE | QACC_STATUS_FAULT;

  qacc_select_queue(QACC_QUEUE1);
  *qacc_reg(QACC_REG_IRQ_ACK) = 1U << QACC_QUEUE1;
  iommu_clear_domain_fault(QACC_DOMAIN_RTOS);
  *qacc_reg(QACC_REG_STATUS) = QACC_STATUS_DONE | QACC_STATUS_FAULT;
  qacc_reset_irq_seen(QACC_VECTOR1);
  __sync_synchronize();
  *qacc_reg(QACC_REG_SRC_LO) = (uint32)qacc.src_iova;
  *qacc_reg(QACC_REG_SRC_HI) = (uint32)(qacc.src_iova >> 32);
  *qacc_reg(QACC_REG_DST_LO) = (uint32)qacc.dst_iova;
  *qacc_reg(QACC_REG_DST_HI) = (uint32)(qacc.dst_iova >> 32);
  *qacc_reg(QACC_REG_LEN) = QACC_TEST_LEN;
  *qacc_reg(QACC_REG_CMD) = QACC_CMD_RUN | QACC_CMD_IRQ;
  __sync_synchronize();

  if(qacc_wait_irq(QACC_VECTOR1, 20) < 0){
    printf("pcie-accel: domain isolation timeout status=%x irq_status=%x\n",
           *qacc_reg(QACC_REG_STATUS), *qacc_reg(QACC_REG_IRQ_STATUS));
    return -1;
  }
  if((*qacc_reg(QACC_REG_STATUS) & QACC_STATUS_FAULT) == 0 ||
     (iommu_domain_status(QACC_DOMAIN_RTOS) & QACC_IOMMU_FAULT) == 0 ||
     qacc_fault_addr(QACC_DOMAIN_RTOS) != qacc.src_iova ||
     (iommu_domain_status(QACC_DOMAIN_XV6) & QACC_IOMMU_FAULT) != 0){
    printf("pcie-accel: domain isolation failed q1_fault=0x%lx q1_iommu=%x q0_iommu=%x\n",
           qacc_fault_addr(QACC_DOMAIN_RTOS),
           iommu_domain_status(QACC_DOMAIN_RTOS),
           iommu_domain_status(QACC_DOMAIN_XV6));
    return -1;
  }

  *qacc_reg(QACC_REG_IRQ_ACK) = 1U << QACC_QUEUE1;
  iommu_clear_domain_fault(QACC_DOMAIN_RTOS);
  *qacc_reg(QACC_REG_STATUS) = QACC_STATUS_DONE | QACC_STATUS_FAULT;
  qacc_select_queue(QACC_QUEUE0);

  printf("pcie-accel: selftest ok len=%d irq_count=%d vectors=%d/%d queues=2 domains=2 bounce=on iommu=on fault=blocked isolation=on\n",
         QACC_TEST_LEN, qacc.irq_count,
         qacc.vector_count[0], qacc.vector_count[1]);
  return 0;
}

int
pcie_accel_benchmark(struct pcie_accel_bench_result *out)
{
  static uint32 sizes[PCIE_BENCH_MAX_SIZES] = { 64, 128, 1024, 4096 };
  static uint32 iters[PCIE_BENCH_MAX_SIZES] = { 128, 128, 64, 32 };
  uint32 base_irq;
  uint64 bad_iova;

  if(!qacc.present)
    return -1;

  memset(out, 0, sizeof(*out));
  out->sizes = PCIE_BENCH_MAX_SIZES;

  qacc_fill_src(QACC_BENCH_BUF_SIZE);
  dma_sync_for_device(&qacc.src_dma);

  for(uint32 s = 0; s < PCIE_BENCH_MAX_SIZES; s++){
    uint64 t0;
    uint64 t1;
    uint32 len = sizes[s];

    memset(qacc.dst_dma.va, 0, qacc.dst_dma.size);
    dma_sync_for_device(&qacc.dst_dma);
    iommu_clear_domain_fault(QACC_DOMAIN_XV6);

    base_irq = qacc.irq_count;
    t0 = r_time();
    for(uint32 i = 0; i < iters[s]; i++){
      if(qacc_run_dma(QACC_QUEUE0, QACC_VECTOR0,
                      qacc.src_iova, qacc.dst_iova, len, 0) < 0)
        return -1;
    }
    t1 = r_time();

    if((iommu_domain_status(QACC_DOMAIN_XV6) & QACC_IOMMU_FAULT) != 0)
      return -1;
    if(qacc_verify_dst(len) < 0)
      return -1;

    out->entries[s].len = len;
    out->entries[s].iters = iters[s];
    out->entries[s].elapsed_ticks = t1 - t0;
    out->entries[s].bytes = (uint64)len * iters[s];
    out->entries[s].irq_delta = qacc.irq_count - base_irq;
  }

  bad_iova = qacc.iova_base + qacc.iova_size + DMA_CACHELINE_SIZE;
  iommu_clear_domain_fault(QACC_DOMAIN_XV6);
  if(qacc_run_dma(QACC_QUEUE0, QACC_VECTOR0,
                  bad_iova, qacc.dst_iova, QACC_TEST_LEN, 1) < 0)
    return -1;
  if((iommu_domain_status(QACC_DOMAIN_XV6) & QACC_IOMMU_FAULT) == 0 ||
     qacc_fault_addr(QACC_DOMAIN_XV6) != bad_iova)
    return -1;
  out->recovery_faults++;

  iommu_clear_domain_fault(QACC_DOMAIN_XV6);
  memset(qacc.dst_dma.va, 0, qacc.dst_dma.size);
  dma_sync_for_device(&qacc.dst_dma);
  if(qacc_run_dma(QACC_QUEUE0, QACC_VECTOR0,
                  qacc.src_iova, qacc.dst_iova, QACC_TEST_LEN, 0) < 0)
    return -1;
  if(qacc_verify_dst(QACC_TEST_LEN) < 0)
    return -1;
  out->recovery_ok = 1;
  out->vector0_count = qacc.vector_count[0];
  out->vector1_count = qacc.vector_count[1];
  out->ok = 1;

  return 0;
}
