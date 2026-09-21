#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/shmem.h"
#include "kernel/pcie_accel.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  struct icc_msg msg;
  struct amp_accel_req req;
  struct amp_accel_resp resp;
  uint32 reply_cmd;

  (void)argc;
  (void)argv;

  printf("accelserv: listening ep=%x\n", SHMEM_EP_XV6_ACCEL);
  for(;;){
    int n = iccrecvmsg(SHMEM_EP_XV6_ACCEL, &msg, 0);

    if(n < 0)
      continue;

    memset(&resp, 0, sizeof(resp));
    resp.type = SHMEM_CMD_ACCEL_ERROR;
    resp.job_id = msg.cookie;
    resp.state = ACCEL_JOB_FAULT;
    resp.status = SHMEM_ACCEL_STATUS_INVAL;

    if(msg.cmd == SHMEM_CMD_ACCEL_SUBMIT &&
       msg.len == sizeof(struct amp_accel_req)){
      memmove(&req, msg.payload, sizeof(req));
      pcieacceljob(&req, &resp);
    }

    reply_cmd = resp.status == SHMEM_ACCEL_STATUS_OK ?
                SHMEM_CMD_ACCEL_COMPLETE : SHMEM_CMD_ACCEL_ERROR;
    if(iccsend(msg.src_ep, reply_cmd, (char*)&resp, sizeof(resp),
               msg.cookie) < 0){
      printf("accelserv: reply failed job=%d status=%d\n",
             resp.job_id, resp.status);
    } else {
      printf("accelserv: job=%d len=%d state=%d status=%d ticks=%ld irq=%d\n",
             resp.job_id, resp.len, resp.state, resp.status,
             resp.elapsed_ticks, resp.irq_count);
    }
  }
}
