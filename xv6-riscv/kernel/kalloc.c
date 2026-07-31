// Buddy physical memory allocator.
// Allocates 2^order contiguous 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define MAX_ORDER 11

struct page {
  struct page *prev;
  struct page *next;
  uint8 order;
  uint8 allocated;
  uint16 _pad;
};

static struct {
  struct spinlock lock;
  struct page *pages;
  uint64 total_pages;
  struct page free_head[MAX_ORDER + 1];
  uint64 free_count[MAX_ORDER + 1];
} buddy;

extern char end[]; // first address after kernel, defined by kernel.ld.

static uint64
pa_to_pfn(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

static uint64
pfn_to_pa(uint64 pfn)
{
  return KERNBASE + pfn * PGSIZE;
}

static void
list_init(struct page *head)
{
  head->next = head;
  head->prev = head;
}

static int
list_empty(struct page *head)
{
  return head->next == head;
}

static void
list_insert(struct page *head, struct page *pg)
{
  pg->next = head->next;
  pg->prev = head;
  head->next->prev = pg;
  head->next = pg;
}

static void
list_remove(struct page *pg)
{
  pg->prev->next = pg->next;
  pg->next->prev = pg->prev;
  pg->next = 0;
  pg->prev = 0;
}

static void
free_insert(uint64 pfn, int order)
{
  struct page *pg = &buddy.pages[pfn];

  pg->order = order;
  pg->allocated = 0;
  list_insert(&buddy.free_head[order], pg);
  buddy.free_count[order]++;
}

static void
free_remove(uint64 pfn, int order)
{
  struct page *pg = &buddy.pages[pfn];

  list_remove(pg);
  buddy.free_count[order]--;
}

static void
add_range(uint64 start_pfn, uint64 end_pfn)
{
  uint64 pfn = start_pfn;

  while(pfn < end_pfn){
    int order;

    for(order = MAX_ORDER; order > 0; order--){
      uint64 pages = 1L << order;
      if((pfn & (pages - 1)) == 0 && pfn + pages <= end_pfn)
        break;
    }
    free_insert(pfn, order);
    pfn += 1L << order;
  }
}

void
kinit()
{
  uint64 phys_pages = (PHYSTOP - KERNBASE) / PGSIZE;
  uint64 pages_bytes = phys_pages * sizeof(struct page);
  uint64 free_start;

  initlock(&buddy.lock, "buddy");
  buddy.pages = (struct page*)PGROUNDUP((uint64)end);
  buddy.total_pages = phys_pages;

  for(int order = 0; order <= MAX_ORDER; order++){
    list_init(&buddy.free_head[order]);
    buddy.free_count[order] = 0;
  }

  for(uint64 i = 0; i < buddy.total_pages; i++){
    buddy.pages[i].prev = 0;
    buddy.pages[i].next = 0;
    buddy.pages[i].order = 0;
    buddy.pages[i].allocated = 1;
  }

  free_start = PGROUNDUP((uint64)buddy.pages + pages_bytes);
  if(free_start < KERNBASE || free_start >= PHYSTOP)
    panic("kinit");

  acquire(&buddy.lock);
  add_range(pa_to_pfn(free_start), pa_to_pfn(PHYSTOP));
  release(&buddy.lock);
}

void *
kalloc_pages(int order)
{
  struct page *pg;
  uint64 pfn;
  int cur;

  if(order < 0 || order > MAX_ORDER)
    return 0;

  acquire(&buddy.lock);
  for(cur = order; cur <= MAX_ORDER; cur++){
    if(!list_empty(&buddy.free_head[cur]))
      break;
  }
  if(cur > MAX_ORDER){
    release(&buddy.lock);
    return 0;
  }

  pg = buddy.free_head[cur].next;
  pfn = pg - buddy.pages;
  free_remove(pfn, cur);

  while(cur > order){
    cur--;
    free_insert(pfn + (1L << cur), cur);
  }

  buddy.pages[pfn].order = order;
  buddy.pages[pfn].allocated = 1;
  release(&buddy.lock);

  memset((void*)pfn_to_pa(pfn), 5, PGSIZE * (1L << order));
  return (void*)pfn_to_pa(pfn);
}

void
kfree_pages(void *pa, int order)
{
  uint64 pfn;
  uint64 block_pages;

  if(pa == 0)
    return;
  if(order < 0 || order > MAX_ORDER)
    panic("kfree_pages");
  if(((uint64)pa % PGSIZE) != 0 || (uint64)pa < KERNBASE || (uint64)pa >= PHYSTOP)
    panic("kfree_pages");

  pfn = pa_to_pfn((uint64)pa);
  block_pages = 1L << order;
  if(pfn + block_pages > buddy.total_pages)
    panic("kfree_pages");

  memset(pa, 1, PGSIZE * block_pages);

  acquire(&buddy.lock);
  if(buddy.pages[pfn].allocated == 0 || buddy.pages[pfn].order != order){
    release(&buddy.lock);
    panic("kfree_pages");
  }

  buddy.pages[pfn].allocated = 0;
  while(order < MAX_ORDER){
    uint64 buddy_pfn = pfn ^ (1L << order);
    struct page *buddy_pg;

    if(buddy_pfn >= buddy.total_pages)
      break;
    buddy_pg = &buddy.pages[buddy_pfn];
    if(buddy_pg->allocated || buddy_pg->order != order)
      break;

    free_remove(buddy_pfn, order);
    if(buddy_pfn < pfn)
      pfn = buddy_pfn;
    order++;
    buddy.pages[pfn].order = order;
    buddy.pages[pfn].allocated = 0;
  }

  free_insert(pfn, order);
  release(&buddy.lock);
}

void *
kalloc(void)
{
  return kalloc_pages(0);
}

void
kfree(void *pa)
{
  kfree_pages(pa, 0);
}
