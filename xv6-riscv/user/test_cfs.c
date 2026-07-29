#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void busy_work() {
  volatile int i;
  for(i = 0; i < 1000000; i++) { }
}

int
main(int argc, char *argv[])
{
  int pid1, pid2, pid3;

  printf("Starting CFS test...\n");

  pid1 = fork();
  if(pid1 == 0){
    int mypid = getpid();
    setnice(mypid, -2);
    uint64 start = uptime();
    int count = 0;
    while(uptime() - start < 300){
      busy_work();
      count++;
    }
    printf("P1 nice=-2 done, count=%d\n", count);
    exit(0);
  }

  pid2 = fork();
  if(pid2 == 0){
    int mypid = getpid();
    setnice(mypid, 0);
    uint64 start = uptime();
    int count = 0;
    while(uptime() - start < 300){
      busy_work();
      count++;
    }
    printf("P2 nice=0 done, count=%d\n", count);
    exit(0);
  }

  pid3 = fork();
  if(pid3 == 0){
    int mypid = getpid();
    setnice(mypid, 2);
    uint64 start = uptime();
    int count = 0;
    while(uptime() - start < 300){
      busy_work();
      count++;
    }
    printf("P3 nice=2 done, count=%d\n", count);
    exit(0);
  }

  wait(0);
  wait(0);
  wait(0);

  printf("CFS test done.\n");
  exit(0);
}