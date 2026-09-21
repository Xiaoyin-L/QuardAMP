# QuardAMP
基于 Quard-Star 的 RISC-V 双系统内核与内存管理增强平台

# 本项目的最终实现结果是
于 Quard-Star/QEMU 和 OpenSBI Domain 构建 RISC-V AMP 异构计算平台，将多核 xv6 与 FreeRTOS 分别运行于独立 HART 和内存域。完成 xv6 的 S-mode 启动、多核 HSM 启动、定时器及设备适配；参考 ChCore 在 xv6 中实现 Buddy 页面分配器和 Slab 小对象分配器；保留共享内存 rpmsg/ICC ring 与 mailbox interrupt 作为 xv6-FreeRTOS 控制面；由 xv6 作为 PCIe accelerator owner 管理 DMA、IOMMU、MSI-X 和 queue/domain 隔离，FreeRTOS 通过 ACCEL_SUBMIT/ACCEL_COMPLETE 控制消息提交实时侧 accelerator job。

旧的 mailbox、shmem、ICC echo/RPC 和 IPC benchmark 程序不再作为项目主功能扩展，保留为 AMP 控制面 regression baseline。当前主线是：

```text
FreeRTOS realtime client
  -> AMP Control Plane
  -> xv6 accelerator service
  -> PCIe Accelerator
  -> DMA / IOMMU / MSI-X
  -> completion back to FreeRTOS
```
