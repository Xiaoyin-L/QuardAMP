#ifndef XV6_SLAB_H
#define XV6_SLAB_H

struct slab_stat_entry {
  uint64 obj_size;
  uint64 objs_per_slab;
  uint64 alloc_count;
  uint64 free_count;
  uint64 active_objs;
  uint64 pages_used;
  uint64 reclaim_count;
};

void  slab_init(void);
void* kmalloc(uint64 size);
void  kfree_obj(void *obj);
int   slab_get_stats(struct slab_stat_entry *buf, int max_entries);

#endif
