#ifndef XV6_PCIE_ACCEL_H
#define XV6_PCIE_ACCEL_H

#define PCIE_BENCH_MAX_SIZES 4

#define ACCEL_JOB_NEW       0
#define ACCEL_JOB_QUEUED    1
#define ACCEL_JOB_RUNNING   2
#define ACCEL_JOB_DONE      3
#define ACCEL_JOB_FAULT     4
#define ACCEL_JOB_TIMEOUT   5
#define ACCEL_JOB_CANCELLED 6

struct pcie_accel_bench_entry {
  uint32 len;
  uint32 iters;
  uint64 elapsed_ticks;
  uint64 bytes;
  uint64 irq_delta;
};

struct pcie_accel_bench_result {
  uint32 ok;
  uint32 sizes;
  uint32 recovery_faults;
  uint32 recovery_ok;
  uint32 vector0_count;
  uint32 vector1_count;
  struct pcie_accel_bench_entry entries[PCIE_BENCH_MAX_SIZES];
};

struct accel_job {
  uint32 job_id;
  uint32 opcode;
  uint64 src_iova;
  uint64 dst_iova;
  uint32 len;
  uint32 state;
  int status;
};

#endif
