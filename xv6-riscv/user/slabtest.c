#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define BATCH 6
#define ROUNDS 5
#define NSTATS 7

int
main(void)
{
  int fds[BATCH][2];
  int total = 0;
  struct slab_stat_entry stats[NSTATS];
  int n;

  printf("=== Slab Allocator Test ===\n");
  printf("plan: %d rounds x %d pipes = %d total alloc/free cycles\n",
         ROUNDS, BATCH, ROUNDS * BATCH);
  printf("each pipe struct uses the 1024 B slab class\n\n");

  for(int r = 0; r < ROUNDS; r++){
    int made = 0;

    for(int i = 0; i < BATCH; i++){
      if(pipe(fds[i]) < 0){
        printf("pipe failed at round=%d index=%d\n", r, i);
        break;
      }
      made++;
      total++;
    }

    for(int i = 0; i < made; i++){
      close(fds[i][0]);
      close(fds[i][1]);
    }
  }

  printf("total pipes created+closed: %d\n\n", total);

  n = slabinfo(stats, NSTATS);
  if(n < 0){
    printf("slabinfo failed\n");
    exit(1);
  }

  printf("--- Slab Statistics ---\n");
  for(int i = 0; i < n; i++){
    if(stats[i].alloc_count == 0 && stats[i].pages_used == 0)
      continue;
    printf("[%ld B] per_slab=%ld allocs=%ld frees=%ld active=%ld pages=%ld reclaim=%ld\n",
           stats[i].obj_size,
           stats[i].objs_per_slab,
           stats[i].alloc_count,
           stats[i].free_count,
           stats[i].active_objs,
           stats[i].pages_used,
           stats[i].reclaim_count);
  }

  for(int i = 0; i < n; i++){
    if(stats[i].obj_size == 1024 && stats[i].alloc_count > 0){
      uint64 without_slab = stats[i].alloc_count;
      uint64 with_slab = (stats[i].alloc_count + stats[i].objs_per_slab - 1) /
                         stats[i].objs_per_slab;
      uint64 saved = without_slab - with_slab;
      uint64 pct = saved * 100 / without_slab;

      printf("\n=== Effectiveness Summary ===\n");
      printf("size 1024 B, %ld allocs, %ld objs/slab:\n",
             stats[i].alloc_count, stats[i].objs_per_slab);
      printf("  without slab: %ld pages (1 page per object)\n", without_slab);
      printf("  with slab: %ld pages (peak)\n", with_slab);
      printf("  pages saved: %ld (%ld percent reduction)\n", saved, pct);
      printf("  pages reclaimed: %ld (returned to buddy)\n", stats[i].reclaim_count);
      printf("  active objs: %ld (0 = no leak)\n", stats[i].active_objs);
      break;
    }
  }

  printf("\n=== Test Complete ===\n");
  exit(0);
}
