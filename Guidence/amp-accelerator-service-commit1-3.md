# AMP Accelerator Service：Commit 1/2/3 实现记录

本文记录本轮将旧 AMP ICC 通道接入 PCIe Accelerator，并升级为 SQ/CQ + doorbell 提交接口的实现过程。它对应原始建议中“四个 commit 开发顺序”的前三个 commit，而不是现有 `pcie-accelerator-stage1.md` 到 `pcie-accelerator-stage5.md` 里的某个已编号 PCIe 阶段。

目标不是删除旧跨核通信，而是把它降级为 xv6 与 FreeRTOS 之间的控制面，让 FreeRTOS 通过轻量消息提交 accelerator job，xv6 继续作为 PCIe owner 负责 DMA、IOMMU、MSI-X 与错误处理。

## 对四个 commit 的完成情况

原始建议是：

```text
Commit 1 - IPC cleanup + accelerator protocol
Commit 2 - FreeRTOS -> xv6 -> PCIe 最小闭环
Commit 3 - Async SQ/CQ + doorbell
Commit 4 - 端到端 benchmark
```

本轮实际完成情况：

```text
[x] Commit 1 - IPC cleanup + accelerator protocol
    已完成：新增 ACCEL_SUBMIT / ACCEL_COMPLETE / ACCEL_ERROR 协议
    已完成：保留 shared ring + mailbox interrupt 作为控制面
    已完成：删除 FreeRTOS 侧旧自动 demo task
    已完成：把旧 mailbox/shmem/ICC/RPC/bench 测试入口降级为 regression baseline

[x] Commit 2 - FreeRTOS -> xv6 -> PCIe 最小闭环
    已完成：RTOS submit
    已完成：xv6 accel service
    已完成：复用现有 PCIe MMIO DMA
    已完成：MSI-X completion
    已完成：xv6 回 RTOS complete
    已完成：QEMU 冒烟验证

[x] Commit 3 - Async SQ/CQ + doorbell
    已完成：QEMU accelerator BAR 新增 SQ/CQ/doorbell 寄存器
    已完成：Submission Ring / Completion Ring
    已完成：producer/consumer index
    已完成：job_id descriptor -> CQE 回传
    已完成：xv6 accelerator service 路径改用 SQ/CQ 提交
    已完成：旧 SRC/DST/LEN/CMD 同步 MMIO 路径保留为 selftest/benchmark regression baseline

[ ] Commit 4 - 端到端 benchmark
    未开始，仅有本轮冒烟日志
```

因此更准确的结论是：

```text
当前完成了 Commit 1、Commit 2、Commit 3；
Commit 4 尚未完成。
```

## 当前阶段定位

仓库现有 PCIe 阶段线是：

```text
PCIe 阶段一：BAR + DMA + MSI/MSI-X 最小路径
PCIe 阶段二：DMA buffer / bounce buffer / cache sync
PCIe 阶段三：IOMMU / IOVA 授权
PCIe 阶段四：多 domain / 多队列 / MSI-X vector 隔离
PCIe 阶段五：性能、稳定性、恢复和 benchmark 最小闭环
```

本轮工作发生在 **PCIe 阶段五之后**，但不是“PCIe 阶段六”的正式文档。更准确的名字是：

```text
AMP Accelerator Service 集成阶段
```

它完成的是：把前面已经做好的 PCIe accelerator 能力接入 AMP 控制面，让 FreeRTOS 能通过 xv6 间接使用 accelerator。

## 当前功能完成度

本轮已经完成：

```text
[x] 扩展 AMP 控制协议：ACCEL_SUBMIT / ACCEL_COMPLETE / ACCEL_ERROR
[x] 新增 FreeRTOS accelerator client endpoint
[x] 新增 xv6 accelerator service endpoint
[x] 打通 FreeRTOS -> xv6 -> PCIe queue0 -> DMA -> MSI-X -> xv6 -> FreeRTOS 最小闭环
[x] 保留旧 rpmsg/ICC ring + mailbox 作为控制面
[x] QEMU 冒烟验证 job submit/complete 成功

[x] Submission Queue / Completion Queue
[x] BAR doorbell 驱动 descriptor ring
[x] job_id 贯穿 RTOS request、xv6 service、PCIe CQE、RTOS complete
[ ] queue1 交给 FreeRTOS 直接驱动
[ ] 端到端 benchmark 系统化
[ ] IOMMU fault/recovery 的跨核错误路径细化
```

所以这次不是完成四个 commit 的全部规划，也不是完成一个正式编号的 `pcie-accelerator-stage6.md`。当前完成的是：

```text
PCIe 阶段五之后的 AMP Accelerator Service + SQ/CQ 提交闭环
```

如果按原始建议里的 4 个开发提交拆分看：

```text
Commit 1 - IPC cleanup + accelerator protocol：完成
Commit 2 - FreeRTOS -> xv6 -> PCIe 最小闭环：完成，并通过 QEMU 冒烟
Commit 3 - Async SQ/CQ + doorbell：完成，并通过 QEMU 冒烟
Commit 4 - 端到端 benchmark：未开始，只做了日志级冒烟验证
```

也就是说，原始建议中的第 1、2、3 步已经完成；第 4 步尚未开始。

## 当前设计

当前结构：

```text
FreeRTOS periodic task
  |
  |  ACCEL_SUBMIT { job_id, opcode, len, src_offset, dst_offset }
  |  over rpmsg/ICC + mailbox
  v
xv6 ICC endpoint: SHMEM_EP_XV6_ACCEL
  |
  |  icc queue
  v
xv6 user daemon: accelserv
  |
  |  pcieacceljob syscall
  v
xv6 kernel pcie_accel_submit_job()
  |
  |  SQ descriptor + BAR doorbell
  v
PCIe Accelerator queue0 / domain0
  |
  |  DMA + IOMMU + CQE + MSI-X
  v
xv6
  |
  |  ACCEL_COMPLETE / ACCEL_ERROR
  v
FreeRTOS completion handler
```

这里有两个关键约束：

1. FreeRTOS 仍然是 accelerator client，不直接拥有 PCIe。
2. xv6 仍然是 accelerator owner，统一负责设备枚举、BAR、IOMMU、MSI-X、DMA buffer 和故障处理。

当前 request/response 只传 descriptor，不搬大块 payload：

```c
struct amp_accel_req {
    uint32_t type;
    uint32_t job_id;
    uint32_t opcode;
    uint32_t len;
    uint64_t src_offset;
    uint64_t dst_offset;
};

struct amp_accel_resp {
    uint32_t type;
    uint32_t job_id;
    uint32_t state;
    int32_t status;
    uint32_t len;
    uint32_t irq_count;
    uint64_t elapsed_ticks;
};
```

当前 `src_offset/dst_offset` 暂时映射到 xv6 PCIe driver 内部已有 DMA bounce buffer。PCIe 设备侧已经使用 descriptor ring 提交，后续如果升级为真正共享 DMA buffer，AMP 协议层可以继续沿用这套 descriptor 语义。

## 主要改动

### 1. 共享 ABI 增加 accelerator 协议

文件：

```text
xv6-riscv/kernel/shmem.h
trusted_domain/driver/shmem.h
```

新增 endpoint：

```text
SHMEM_EP_XV6_ACCEL
SHMEM_EP_RTOS_ACCEL
```

新增命令：

```text
SHMEM_CMD_ACCEL_SUBMIT
SHMEM_CMD_ACCEL_COMPLETE
SHMEM_CMD_ACCEL_ERROR
```

新增状态：

```text
SHMEM_ACCEL_STATUS_OK
SHMEM_ACCEL_STATUS_INVAL
SHMEM_ACCEL_STATUS_NO_DEVICE
SHMEM_ACCEL_STATUS_FAULT
SHMEM_ACCEL_STATUS_TIMEOUT
```

### 2. xv6 ICC 注册 accelerator endpoint

文件：

```text
xv6-riscv/kernel/icc.c
```

新增：

```c
icc_eps[1].ep = SHMEM_EP_XV6_ACCEL;
```

这样 mailbox ISR 收到 FreeRTOS 发来的 `ACCEL_SUBMIT` 后，只负责把消息放进 ICC endpoint queue。真正处理放到进程上下文。

### 3. xv6 PCIe driver 增加 job 提交入口

文件：

```text
xv6-riscv/kernel/pci_accel.c
xv6-riscv/kernel/pcie_accel.h
```

新增：

```c
int pcie_accel_submit_job(struct amp_accel_req *req,
                          struct amp_accel_resp *resp);
```

它把 `amp_accel_req` 转成内部 `accel_job`，复用现有：

```text
queue0
domain0
qacc_submit_desc()
Submission Ring
Completion Ring
BAR doorbell
IOMMU fault check
MSI-X IRQ wait
DMA output verify
```

旧的 `qacc_run_dma()` 仍然保留，供 `pcie_accel_selftest()` 和 `pcie_accel_benchmark()` 作为 regression baseline 使用。

当前 job state 包括：

```text
NEW
QUEUED
RUNNING
DONE
FAULT
TIMEOUT
CANCELLED
```

### 4. xv6 新增用户态 accelerator service

文件：

```text
xv6-riscv/user/accelserv.c
xv6-riscv/user/init.c
xv6-riscv/Makefile
```

`accelserv` 由 `init` 自动后台启动：

```text
init
  -> fork
  -> exec("accelserv")
```

它阻塞等待：

```text
SHMEM_EP_XV6_ACCEL
```

收到 `ACCEL_SUBMIT` 后：

```text
iccrecvmsg()
  -> pcieacceljob()
  -> iccsend(ACCEL_COMPLETE / ACCEL_ERROR)
```

### 5. xv6 新增 syscall

文件：

```text
xv6-riscv/kernel/sysproc.c
xv6-riscv/kernel/syscall.c
xv6-riscv/kernel/syscall.h
xv6-riscv/user/user.h
xv6-riscv/user/usys.pl
```

新增两个 syscall：

```text
iccrecvmsg(ep, msg, timeout_ms)
pcieacceljob(req, resp)
```

为什么需要 `iccrecvmsg()`：

原来的 `iccrecv()` 只把 payload 拷给用户态，不包含：

```text
src_ep
dst_ep
cmd
cookie
flags
```

但是 `accelserv` 回包必须知道 FreeRTOS 的 `src_ep` 和 `cookie`，所以新增完整消息接收接口。

### 6. FreeRTOS 新增 accelerator client

文件：

```text
trusted_domain/main.c
trusted_domain/driver/icc.c
trusted_domain/driver/icc.h
```

新增：

```text
vAccelClientTask
icc_accel_complete_handler
```

`vAccelClientTask` 周期发送：

```text
ACCEL_SUBMIT job=1,2,... len=128 opcode=XOR
```

`icc_accel_complete_handler()` 接收并打印：

```text
job_id
cmd
state
status
len
elapsed_ticks
irq_count
```

同时 rpmsg namespace 公告增加：

```text
quardamp-accel-client
```

### 7. 旧 demo 降级为 regression baseline

本轮删除了 FreeRTOS 侧旧的自动 demo task：

```text
task1 / task2
vMailboxTestTask
vIccTestTask
QUARDAMP_DEMO 开关
```

这些任务原来用于自动验证“能打印、能按 mailbox、能发一条 rpmsg”。在 accelerator service 成为主线后，它们不再作为默认运行负载保留。

xv6 用户态测试程序继续保留，但语义从 demo 改为 regression baseline：

```text
mailboxtest  - raw mailbox doorbell regression
shmemtest    - legacy shmemsend compatibility regression
icctest      - ICC echo request/reply regression
rpctest      - ICC RPC request/reply regression
benchtest    - AMP ICC latency/throughput baseline
pcietest     - PCIe accelerator function/isolation regression
pciebench    - PCIe accelerator benchmark/recovery baseline
accelserv    - AMP accelerator service daemon
```

这样旧跨核通信没有被删除，而是明确降级为：

```text
AMP Control Plane regression baseline
```

### 8. QEMU accelerator 增加 SQ/CQ + doorbell

文件：

```text
qemu-10.2.4/hw/misc/quardamp_accel.c
```

BAR0 新增寄存器：

```text
QACC_REG_SQ_BASE_LO / QACC_REG_SQ_BASE_HI
QACC_REG_SQ_SIZE
QACC_REG_SQ_TAIL
QACC_REG_CQ_BASE_LO / QACC_REG_CQ_BASE_HI
QACC_REG_CQ_SIZE
QACC_REG_CQ_HEAD
QACC_REG_CQ_TAIL
QACC_REG_DOORBELL
```

descriptor/CQE 格式：

```text
SQ descriptor:
  src_iova
  dst_iova
  len
  opcode
  flags
  job_id

CQ entry:
  job_id
  status
  len
  flags
```

设备行为：

```text
guest writes SQ descriptor
guest updates SQ_TAIL
guest writes DOORBELL
QEMU device drains SQ from internal sq_head to sq_tail
QEMU device executes XOR DMA through existing IOMMU translation
QEMU device writes CQE at cq_tail
QEMU device raises MSI-X vector
guest reads CQE and advances CQ_HEAD
```

当前 QEMU 设备模型在 doorbell 写入时立即 drain SQ，因此“硬件执行”仍是模拟器内同步完成；但 guest-visible 接口已经是 SQ/CQ/doorbell，不再要求服务入口直接写 `SRC/DST/LEN/CMD`。

## 遇到的问题：不能在 mailbox 中断路径里等待 PCIe MSI-X

### 问题现象

最初设计时，曾考虑在 xv6 `icc_notify_recv()` 收到 `SHMEM_EP_XV6_ACCEL` 消息后直接调用：

```text
pcie_accel_submit_job()
  -> qacc_run_dma()
  -> qacc_wait_irq()
```

这看起来路径更短，但有一个隐蔽时序问题：

```text
ICC 收包本身是在 mailbox 中断路径里触发的
```

而 `qacc_run_dma()` 当前会启动 PCIe accelerator，然后同步等待 MSI-X completion。

### 根因

如果在 mailbox ISR 路径中同步等待 PCIe MSI-X，就会变成：

```text
mailbox IRQ handler
  -> ICC drain
  -> accelerator service
  -> submit PCIe job
  -> wait MSI-X IRQ
```

这会导致两个风险：

1. 当前 hart 仍处于中断处理链路中，等待另一个外部中断不可靠。
2. mailbox ISR 被长时间占用，后续 ICC/rpmsg 通知可能无法及时处理。

更严重时会形成：

```text
正在处理一个中断
  -> 等另一个中断完成
  -> 另一个中断无法按预期推进
```

因此这不是简单性能问题，而是中断上下文设计错误。

### 定位方法

定位这个问题时主要看调用链：

```text
mailboxintr()
  -> icc_notify_recv()
  -> 如果直接处理 ACCEL_SUBMIT
  -> pcie_accel_submit_job()
  -> qacc_wait_irq()
```

判断标准：

```text
任何可能 sleep / busy wait / 等待另一个 IRQ 的操作，都不应该放在 mailbox ISR 收包路径里。
```

### 解决方法

最终实现改成：

```text
mailbox ISR
  -> icc_notify_recv()
  -> 只把消息入队到 SHMEM_EP_XV6_ACCEL
  -> 立即返回

xv6 user process accelserv
  -> iccrecvmsg()
  -> pcieacceljob()
  -> iccsend()
```

也就是：

```text
中断上下文只做搬运
进程上下文做真正服务处理
```

这个解决方式和 FreeRTOS 侧之前 UART RX 的改造原则一致：

```text
ISR 里只投递事件
任务/进程上下文里处理复杂逻辑
```

## 后续需要重点防范的同类问题

这些问题都属于“看起来能跑，但在中断、DMA、队列或并发压力下容易变成隐蔽死锁/错包/误完成”的问题。后续 Commit 4 和继续扩展 queue/domain benchmark 时，应优先按这些方向检查。

### 1. MSI-X ISR 里不要反向发送 ICC completion

风险：

```text
PCIe MSI-X IRQ
  -> pcie_accel_intr()
  -> 如果直接 iccsend()
  -> 可能反向占用 ICC/mailbox 路径
```

后续如果把 completion 处理进一步异步化，应保持：

```text
MSI-X ISR 只 ack / 记录状态 / 唤醒等待者
accelserv 进程上下文消费 CQE 并发送 ACCEL_COMPLETE/ACCEL_ERROR
```

定位方法：

```text
检查 pcie_accel_intr() 调用链，确认其中没有 iccsend()/copyout()/长循环/等待锁。
```

解决方法：

```text
ISR 中只做最小化状态更新；所有可能阻塞、等待、跨子系统发送的动作放到进程上下文。
```

### 2. SQ/CQ 的 producer/consumer 顺序必须固定

风险：

如果 guest 先敲 doorbell，再把 descriptor 完整写入 SQ，设备可能读到半旧半新的 job；如果设备先发 MSI-X，再把 CQE 写完整，guest 可能读到旧 completion。

正确顺序：

```text
submit:
  fill SQ descriptor
  dma_sync_for_device()
  update SQ_TAIL
  write DOORBELL

complete:
  device writes CQE
  device updates CQ_TAIL
  device raises MSI-X
  guest sees IRQ
  dma_sync_for_cpu()
  guest checks CQ_TAIL != CQ_HEAD
  guest consumes CQE
  guest updates CQ_HEAD
```

定位方法：

```text
对比 job_id、SQ_TAIL、CQ_HEAD、CQ_TAIL。
若 irq=1 但 CQ_TAIL 没前进，优先查设备写 CQE/更新 CQ_TAIL/触发 MSI-X 的顺序。
若 CQE job_id 偶发错误，优先查 descriptor 写入和 cache sync 顺序。
```

解决方法：

```text
把 ring descriptor 填写、tail 更新、doorbell、CQE 写入、IRQ 触发视为一个严格协议，不在中间插入可重入路径。
```

### 3. 批量提交前必须补 ring full 与 outstanding job 管理

当前最小闭环实际按单 outstanding job 使用；后续做批量提交时，如果只递增 `SQ_TAIL`，可能覆盖设备还没消费的 SQ slot。

定位方法：

```text
观察 SQ_TAIL 追上 SQ_HEAD 的场景。
若多个请求返回同一个 job_id 或 completion 顺序错乱，优先查 pending job 表。
```

解决方法：

```text
提交前检查 SQ 空位。
为每个 job_id 建立 pending 表。
CQE 到达时按 job_id 完成对应请求，而不是默认完成当前请求。
```

### 4. queue/domain 归属不能混用

目前 queue0/domain0 是 xv6 管理的 PCIe accelerator 闭环；queue1/domain1 应作为后续 RTOS direct / 双 domain benchmark 的扩展方向。

风险：

```text
FreeRTOS 直接写 queue0
xv6 accelserv 同时写 queue0
IOMMU domain/vector 仍按单 owner 假设配置
```

这会导致 ring owner 不明确、completion vector 不明确，甚至把一个 domain 的 fault 误判成另一个 domain 的 job 失败。

定位方法：

```text
检查 queue_id -> domain_id -> MSI-X vector -> 服务端 owner 的绑定关系。
出现跨域 fault 或 completion 丢失时，先确认请求是否走了预期 queue。
```

解决方法：

```text
每个 queue 只允许一个 producer owner。
双 domain benchmark 前先固化 queue1/domain1 的寄存器、IOMMU 映射和 MSI-X vector 归属。
```

### 5. IOMMU fault 状态不能当作普通状态位长期保留

风险：

一次坏 DMA 留下的 fault 状态如果没有清理，后续正常 job 可能被误判失败；反过来，如果 completion 只看 CQE `status`，也可能漏掉 IOMMU fault。

定位方法：

```text
每次 ACCEL_ERROR 同时记录 job_id、queue_id、domain_id、fault iova、fault cause。
若同一个 fault 地址重复出现，检查 fault clear/reset 顺序。
```

解决方法：

```text
提交前确认 domain 映射有效。
失败路径同时读取并清理对应 domain fault。
把设备 CQE error 与 IOMMU fault 都映射为 ACCEL_ERROR，但日志中保留来源。
```

### 6. reset/reinit 不能只重置设备寄存器

后续实现 reset/reinit 时，如果只 reset QEMU 设备或只 reset guest driver，会留下旧 CQE、旧 pending job、旧 IRQ 状态。

定位方法：

```text
reset 后第一个 job 如果立即完成、job_id 对不上或 CQ_HEAD/CQ_TAIL 非零，优先查 stale CQE/IRQ/pending 表。
```

解决方法：

```text
停止新提交。
把 pending job 全部完成为 ACCEL_ERROR。
清空 SQ/CQ head/tail。
清理 IRQ pending 与 IOMMU fault。
重新写 ring base/size。
最后再开放提交。
```

## 验证结果

构建：

```text
make -C xv6-riscv kernel/kernel fs.img
make -C qemu-10.2.4/build -j4
make CROSS_COMPILE=riscv64-linux-gnu-   # in trusted_domain
```

重新合成 `output/fw/fw.bin` 后，QEMU 冒烟运行 65 秒。

xv6 UART0 日志关键输出：

```text
pcie-accel: ... dma_src=0x89f00000 dma_dst=0x89f01000 sq=0x89f02000 cq=0x89f02100 iova=[0x10000000,0x10004000)
accelserv: listening ep=110
rpmsg ns: quardamp-accel-client addr=10400 flags=0
accelserv: job=1 len=128 state=3 status=0 ticks=73679 irq=1
```

FreeRTOS UART2 日志关键输出：

```text
accel client: submit job=0x1 len=128
accel client: job=0x1 cmd=0x601 state=3 status=0 len=128 ticks=0x11fcf irq=1
```

其中：

```text
cmd=0x601  -> SHMEM_CMD_ACCEL_COMPLETE
state=3   -> ACCEL_JOB_DONE
status=0  -> SHMEM_ACCEL_STATUS_OK
irq=1     -> 每个 job 收到一次 PCIe completion interrupt
```

说明当前 SQ/CQ + doorbell 提交闭环已经跑通。

## 后续计划

下一阶段应继续做 Commit 4：

系统化 benchmark：

```text
RTOS request latency
PCIe submit latency
DMA throughput
MSI-X completion latency
round-trip latency
deadline jitter
fault/recovery latency
```

同时保留更后续方向：

```text
批量提交
reset/reinit
soak test
RTOS queue1 双 domain benchmark
```
