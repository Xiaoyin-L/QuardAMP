#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int cookie = 0x33;

  if(argc > 1)
    cookie = atoi(argv[1]);

  printf("shmemtest: send shared-memory message, cookie=0x%x\n", cookie);

  if(shmemsend(cookie) < 0){
    printf("shmemtest: shmemsend failed\n");
    exit(1);
  }

  printf("shmemtest: done; reply should arrive through mailbox irq\n");
  exit(0);
}
