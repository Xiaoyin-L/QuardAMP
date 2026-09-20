#include "types.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "dma.h"

struct dma_arena {
  struct spinlock lock;
  uint64 next;
  uint64 end;
  uint initialized;
};

static struct dma_arena dma_arena;

static uint64
align_up(uint64 value, uint64 align)
{
  return (value + align - 1) & ~(align - 1);
}

void
dma_init(void)
{
  initlock(&dma_arena.lock, "dma");
  dma_arena.next = DMA_CARVEOUT_BASE;
  dma_arena.end = DMA_CARVEOUT_END;
  dma_arena.initialized = 1;
  memset((void*)DMA_CARVEOUT_BASE, 0, DMA_CARVEOUT_SIZE);
  printf("dma: carveout [0x%lx, 0x%lx) size=%d\n",
         (uint64)DMA_CARVEOUT_BASE, (uint64)DMA_CARVEOUT_END,
         DMA_CARVEOUT_SIZE);
}

int
dma_alloc(struct dma_buf *buf, uint size, uint align)
{
  uint64 pa;

  if(buf == 0 || size == 0 || !dma_arena.initialized)
    return -1;

  if(align < DMA_CACHELINE_SIZE)
    align = DMA_CACHELINE_SIZE;
  if((align & (align - 1)) != 0)
    return -1;

  acquire(&dma_arena.lock);
  pa = align_up(dma_arena.next, align);
  if(pa + size > dma_arena.end){
    release(&dma_arena.lock);
    return -1;
  }
  dma_arena.next = pa + size;
  release(&dma_arena.lock);

  memset((void*)pa, 0, size);
  buf->va = (void*)pa;
  buf->pa = pa;
  buf->size = size;
  return 0;
}

void
dma_sync_for_device(struct dma_buf *buf)
{
  (void)buf;
  __sync_synchronize();
  asm volatile("fence rw, rw" : : : "memory");
}

void
dma_sync_for_cpu(struct dma_buf *buf)
{
  (void)buf;
  asm volatile("fence rw, rw" : : : "memory");
  __sync_synchronize();
}

int
dma_bounce_to_device(struct dma_buf *buf, const void *src, uint len)
{
  if(buf == 0 || src == 0 || len > buf->size)
    return -1;

  memmove(buf->va, src, len);
  dma_sync_for_device(buf);
  return 0;
}

int
dma_bounce_from_device(void *dst, struct dma_buf *buf, uint len)
{
  if(buf == 0 || dst == 0 || len > buf->size)
    return -1;

  dma_sync_for_cpu(buf);
  memmove(dst, buf->va, len);
  return 0;
}
