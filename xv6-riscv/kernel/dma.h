#ifndef XV6_DMA_H
#define XV6_DMA_H

#include "types.h"

struct dma_buf {
  void *va;
  uint64 pa;
  uint size;
};

void dma_init(void);
int dma_alloc(struct dma_buf *buf, uint size, uint align);
void dma_sync_for_device(struct dma_buf *buf);
void dma_sync_for_cpu(struct dma_buf *buf);
int dma_bounce_to_device(struct dma_buf *buf, const void *src, uint len);
int dma_bounce_from_device(void *dst, struct dma_buf *buf, uint len);

#endif
