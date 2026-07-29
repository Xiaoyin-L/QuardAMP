#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NWORKERS 8
#define RUN_TICKS 400

static volatile int sink = 0;

void
busy_work(int loops)
{
  volatile int i;
  for(i = 0; i < loops; i++){
    sink += i;
  }
}

void
worker(int nice, int runtime_ticks, char *name)
{
  int mypid = getpid();
  uint64 start;
  int count = 0;

  if(setnice(mypid, nice) < 0){
    printf("setnice failed: pid=%d nice=%d\n", mypid, nice);
    exit(1);
  }

  start = uptime();
  while(uptime() - start < runtime_ticks){
    busy_work(200000);
    count++;
  }

  printf("worker name=%s pid=%d nice=%d count=%d\n",
         name, mypid, nice, count);
  exit(0);
}

int
main(int argc, char *argv[])
{
  int i, pid;

  // 两高、两中、两低，再补两个中权重，方便观察多核调度
  int nice_table[NWORKERS] = {-2, -2, 0, 0, 2, 2, 0, 0};
  char *name_table[NWORKERS] = {
    "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7"
  };

  printf("=== multi-core CFS CPU-bound test start ===\n");
  printf("workers=%d runtime=%d ticks\n", NWORKERS, RUN_TICKS);

  for(i = 0; i < NWORKERS; i++){
    pid = fork();
    if(pid < 0){
      printf("fork failed at worker %d\n", i);
      exit(1);
    }
    if(pid == 0){
      worker(nice_table[i], RUN_TICKS, name_table[i]);
    }
  }

  for(i = 0; i < NWORKERS; i++){
    wait(0);
  }

  printf("=== multi-core CFS CPU-bound test done ===\n");
  exit(0);
}