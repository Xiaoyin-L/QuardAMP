#include "kernel/types.h"
#include "kernel/pcie_accel.h"
#include "user/user.h"

#define TIMEBASE_HZ 10000000ULL

static uint64
ticks_to_us(uint64 ticks)
{
  return (ticks * 1000000ULL) / TIMEBASE_HZ;
}

int
main(int argc, char **argv)
{
  struct pcie_accel_bench_result result;

  (void)argc;
  (void)argv;

  printf("pciebench: run PCIe accelerator performance/stability benchmark\n");
  if(pcieaccelbench(&result) < 0 || result.ok == 0){
    printf("pciebench: failed\n");
    exit(1);
  }

  for(uint32 i = 0; i < result.sizes; i++){
    struct pcie_accel_bench_entry *e = &result.entries[i];
    uint64 us = ticks_to_us(e->elapsed_ticks);
    uint64 ops_per_sec = e->elapsed_ticks ?
      ((uint64)e->iters * TIMEBASE_HZ) / e->elapsed_ticks : 0;
    uint64 kb_per_sec = e->elapsed_ticks ?
      (e->bytes * TIMEBASE_HZ) / e->elapsed_ticks / 1024 : 0;
    uint64 avg_us = e->iters ? us / e->iters : 0;

    printf("  len=%d iters=%d elapsed=%luus avg=%luus rate=%lu op/s bw=%lu KB/s irq=%lu\n",
           e->len, e->iters, us, avg_us, ops_per_sec, kb_per_sec,
           e->irq_delta);
  }

  printf("  recovery: faults=%d recovered=%d vectors=%d/%d\n",
         result.recovery_faults, result.recovery_ok,
         result.vector0_count, result.vector1_count);
  printf("pciebench: passed\n");
  exit(0);
}
