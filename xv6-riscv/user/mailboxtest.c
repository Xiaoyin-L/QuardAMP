/*
 * mailboxtest —— legacy mailbox doorbell regression test.
 *
 * 用法：
 *   mailboxtest            # 使用默认 reason 0x5aa5
 *   mailboxtest <reason>   # 使用指定十进制 reason
 *
 * 路径：用户态 -> mailboxring() 系统调用(SYS_mailboxring)
 *       -> 内核 sys_mailboxring() -> mailbox_ring_to_rtos()
 *       -> 写 MAILBOX_TX_TO_RTOS(0x10004000) MMIO
 *       -> QEMU 设备锁存 reason、拉起 PLIC 源 13
 *       -> FreeRTOS(hart7) handle_interrupt() 打印
 *          "mailbox irq received, reason=..."（serial2）。
 *
 * This is no longer the project workload path.  Keep it as a low-level
 * regression test for the raw notification device used by the AMP control
 * plane.
 */
#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int reason = 0x5aa5;

  if(argc > 1)
    reason = atoi(argv[1]);

  printf("mailboxtest: ring doorbell to FreeRTOS, reason=0x%x\n", reason);

  if(mailboxring(reason) < 0){
    printf("mailboxtest: mailboxring failed\n");
    exit(1);
  }

  printf("mailboxtest: done\n");
  exit(0);
}
