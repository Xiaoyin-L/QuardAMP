#include "kernel/types.h"
#include "user/user.h"

/*
 * Stage 5 compatibility smoke test.
 *
 * shmemsend() is kept as the older user entry, but its kernel backend now
 * calls icc_send().  Replies are no longer consumed by shmem.c directly:
 * mailboxintr() enters icc_notify_recv(), which dispatches to the local
 * endpoint and wakes a process if one is blocked in iccrecv().
 *
 * Use icctest for the full synchronous user-space request/reply path.
 */
int
main(int argc, char *argv[])
{
  int cookie = 0x33;

  if(argc > 1)
    cookie = atoi(argv[1]);

  printf("shmemtest: send ICC shared-memory message, cookie=0x%x\n", cookie);

  if(shmemsend(cookie) < 0){
    printf("shmemtest: shmemsend failed\n");
    exit(1);
  }

  printf("shmemtest: sent; reply is dispatched by the ICC layer\n");
  exit(0);
}
