#include "kernel/types.h"
#include "user/user.h"

/*
 * Stage 4 ICC smoke test.
 *
 * Path:
 *   user shmemsend(cookie)
 *   -> xv6 shmem_send_to_rtos() publishes to the to_rtos ring
 *   -> mailbox reason 0 notifies FreeRTOS
 *   -> FreeRTOS ISR drains the ring into the ICC dispatch queue
 *   -> vIccDispatchTask runs icc_echo_handler()
 *   -> loan/send publishes the ack to the to_xv6 ring
 *   -> mailbox reason 0 notifies xv6, which consumes the reply.
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

  printf("shmemtest: done; ICC reply should arrive through mailbox irq\n");
  exit(0);
}
