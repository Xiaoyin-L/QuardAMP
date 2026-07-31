#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "slab.h"

#define NSIZECLASS 7
#define SLAB_MAGIC 0x51ab51ab

struct slab_header {
  uint magic;
  int cache_idx;
  int obj_size;
  int total_objs;
  int free_count;
  void *free_list;
  struct slab_header *next;
  struct slab_header *prev;
};

struct slab_cache {
  int obj_size;
  int objs_per_slab;
  struct spinlock lock;
  struct slab_header *slabs;
  int total_slabs;

  uint64 stat_alloc;
  uint64 stat_free;
  uint64 stat_active;
  uint64 stat_pages_used;
  uint64 stat_reclaim;
};

static int size_classes[NSIZECLASS] = {32, 64, 128, 256, 512, 1024, 2048};
static struct slab_cache caches[NSIZECLASS];

static uint64
align_up(uint64 n, uint64 align)
{
  return (n + align - 1) & ~(align - 1);
}

static int
size_to_idx(uint64 size)
{
  for(int i = 0; i < NSIZECLASS; i++){
    if(size <= (uint64)size_classes[i])
      return i;
  }
  return -1;
}

static struct slab_header *
obj_to_slab(void *obj)
{
  return (struct slab_header*)((uint64)obj & ~(PGSIZE - 1));
}

static void
cache_insert_slab(struct slab_cache *cache, struct slab_header *hdr)
{
  hdr->prev = 0;
  hdr->next = cache->slabs;
  if(cache->slabs)
    cache->slabs->prev = hdr;
  cache->slabs = hdr;
}

static void
cache_remove_slab(struct slab_cache *cache, struct slab_header *hdr)
{
  if(hdr->prev)
    hdr->prev->next = hdr->next;
  else
    cache->slabs = hdr->next;
  if(hdr->next)
    hdr->next->prev = hdr->prev;
  hdr->prev = 0;
  hdr->next = 0;
}

static struct slab_header *
slab_create(struct slab_cache *cache, int idx)
{
  struct slab_header *hdr;
  char *slot;
  uint64 offset;

  hdr = (struct slab_header*)kalloc();
  if(hdr == 0)
    return 0;

  hdr->magic = SLAB_MAGIC;
  hdr->cache_idx = idx;
  hdr->obj_size = cache->obj_size;
  hdr->total_objs = cache->objs_per_slab;
  hdr->free_count = cache->objs_per_slab;
  hdr->free_list = 0;
  hdr->next = 0;
  hdr->prev = 0;

  offset = align_up(sizeof(struct slab_header), sizeof(void*));
  for(int i = 0; i < cache->objs_per_slab; i++){
    slot = (char*)hdr + offset + i * cache->obj_size;
    *(void**)slot = hdr->free_list;
    hdr->free_list = slot;
  }

  return hdr;
}

void
slab_init(void)
{
  for(int i = 0; i < NSIZECLASS; i++){
    caches[i].obj_size = size_classes[i];
    caches[i].objs_per_slab =
      (PGSIZE - align_up(sizeof(struct slab_header), sizeof(void*))) / size_classes[i];
    caches[i].slabs = 0;
    caches[i].total_slabs = 0;
    caches[i].stat_alloc = 0;
    caches[i].stat_free = 0;
    caches[i].stat_active = 0;
    caches[i].stat_pages_used = 0;
    caches[i].stat_reclaim = 0;
    initlock(&caches[i].lock, "slab");
  }
}

void *
kmalloc(uint64 size)
{
  struct slab_cache *cache;
  struct slab_header *hdr;
  void *obj;
  int idx;

  if(size == 0)
    return 0;

  idx = size_to_idx(size);
  if(idx < 0)
    return 0;

  cache = &caches[idx];
  acquire(&cache->lock);

  hdr = cache->slabs;
  while(hdr && hdr->free_count == 0)
    hdr = hdr->next;

  if(hdr == 0){
    hdr = slab_create(cache, idx);
    if(hdr == 0){
      release(&cache->lock);
      return 0;
    }
    cache_insert_slab(cache, hdr);
    cache->total_slabs++;
    cache->stat_pages_used++;
  }

  obj = hdr->free_list;
  hdr->free_list = *(void**)obj;
  hdr->free_count--;

  cache->stat_alloc++;
  cache->stat_active++;
  release(&cache->lock);

  memset(obj, 0, cache->obj_size);
  return obj;
}

void
kfree_obj(void *obj)
{
  struct slab_header *hdr;
  struct slab_cache *cache;

  if(obj == 0)
    return;

  hdr = obj_to_slab(obj);
  if(hdr->magic != SLAB_MAGIC)
    panic("kfree_obj");
  if((uint64)obj < (uint64)hdr + align_up(sizeof(struct slab_header), sizeof(void*)))
    panic("kfree_obj");
  if(((uint64)obj - ((uint64)hdr + align_up(sizeof(struct slab_header), sizeof(void*)))) %
     hdr->obj_size)
    panic("kfree_obj");

  cache = &caches[hdr->cache_idx];
  acquire(&cache->lock);

  *(void**)obj = hdr->free_list;
  hdr->free_list = obj;
  hdr->free_count++;
  if(hdr->free_count > hdr->total_objs){
    release(&cache->lock);
    panic("kfree_obj");
  }

  cache->stat_free++;
  if(cache->stat_active > 0)
    cache->stat_active--;

  if(hdr->free_count == hdr->total_objs && cache->total_slabs > 1){
    cache_remove_slab(cache, hdr);
    cache->total_slabs--;
    cache->stat_pages_used--;
    cache->stat_reclaim++;
    hdr->magic = 0;
    release(&cache->lock);
    kfree((void*)hdr);
    return;
  }

  release(&cache->lock);
}

int
slab_get_stats(struct slab_stat_entry *buf, int max_entries)
{
  int count = 0;

  for(int i = 0; i < NSIZECLASS && count < max_entries; i++){
    acquire(&caches[i].lock);
    buf[count].obj_size = caches[i].obj_size;
    buf[count].objs_per_slab = caches[i].objs_per_slab;
    buf[count].alloc_count = caches[i].stat_alloc;
    buf[count].free_count = caches[i].stat_free;
    buf[count].active_objs = caches[i].stat_active;
    buf[count].pages_used = caches[i].stat_pages_used;
    buf[count].reclaim_count = caches[i].stat_reclaim;
    release(&caches[i].lock);
    count++;
  }

  return count;
}
