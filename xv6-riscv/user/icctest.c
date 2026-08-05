#include "kernel/types.h"
#include "kernel/shmem.h"
#include "user/user.h"

/*
 * icctest -- stage-5 synchronous ICC user test.
 *
 * Usage:
 *   icctest             # default cookie 0x5001
 *   icctest <cookie>    # decimal cookie
 *
 * This is the first user-visible receive path:
 *   iccsend() -> sys_iccsend() -> icc_send()
 *     -> shared to_rtos ring + mailbox to FreeRTOS
 *     -> FreeRTOS echo handler replies through to_xv6
 *   iccrecv() -> sys_iccrecv() -> icc_recv()
 *     -> sleep until icc_notify_recv() dispatches a message and wakes us
 *     -> copy payload back to user space.
 */
int
main(int argc, char *argv[])
{
  int cookie = 0x5001;
  char payload[] = "xv6->rtos phase5 hello";
  char buf[SHMSG_PAYLOAD_SIZE + 1];
  int n;

  if(argc > 1)
    cookie = atoi(argv[1]);

  printf("icctest: send to FreeRTOS, cookie=0x%x\n", cookie);
  if(iccsend(SHMEM_EP_RTOS_ECHO, 0x300, payload, strlen(payload), cookie) < 0){
    printf("icctest: iccsend failed\n");
    exit(1);
  }

  printf("icctest: sent, waiting for reply...\n");
  n = iccrecv(SHMEM_EP_XV6_TEST, buf, SHMSG_PAYLOAD_SIZE);
  if(n < 0){
    printf("icctest: iccrecv failed\n");
    exit(1);
  }

  buf[n] = '\0';
  printf("icctest: recv reply payload=%s len=%d\n", buf, n);
  exit(0);
}
