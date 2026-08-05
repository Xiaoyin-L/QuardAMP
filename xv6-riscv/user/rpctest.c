#include "kernel/types.h"
#include "kernel/shmem.h"
#include "user/user.h"

/*
 * rpctest -- stage-6 synchronous RPC user test.
 *
 * Stage 5 exposes iccsend() and iccrecv() as two independent syscalls.  That is
 * enough for a single manual test, but user space must pick cookies and pair
 * replies by itself.  Stage 6 folds the request/reply sequence into rpccall():
 *   user rpccall()
 *     -> sys_rpccall()
 *     -> icc_rpc_call() allocates a private cookie and pending slot
 *     -> icc_send() publishes the request to FreeRTOS
 *     -> icc_notify_recv() matches the reply cookie and wakes the caller
 *
 * This test calls two FreeRTOS endpoints:
 *   - SHMEM_EP_RTOS_UPPER converts "hello rpc" to upper case.
 *   - SHMEM_EP_RTOS_ECHO proves older echo services still work through RPC.
 */
static int
call_and_print(int ep, const char *name, char *req)
{
  char reply[SHMSG_PAYLOAD_SIZE + 1];
  int n;

  memset(reply, 0, sizeof(reply));
  printf("rpctest: rpc_call to %s, req=%s\n", name, req);

  n = rpccall(ep, 0x400, req, strlen(req), reply, SHMSG_PAYLOAD_SIZE);
  if(n < 0){
    printf("rpctest: rpc_call (%s) failed\n", name);
    return -1;
  }

  if(n > SHMSG_PAYLOAD_SIZE)
    n = SHMSG_PAYLOAD_SIZE;
  reply[n] = '\0';
  printf("rpctest: %s reply=%s len=%d\n", name, reply, n);
  return 0;
}

int
main(int argc, char *argv[])
{
  char req_upper[] = "hello rpc";
  char req_echo[] = "hello echo";

  (void)argc;
  (void)argv;

  if(call_and_print(SHMEM_EP_RTOS_UPPER, "upper", req_upper) < 0)
    exit(1);

  if(call_and_print(SHMEM_EP_RTOS_ECHO, "echo", req_echo) < 0)
    exit(1);

  exit(0);
}
