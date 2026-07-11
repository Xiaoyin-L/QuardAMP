# QuardAMP
基于 Quard-Star 的 RISC-V 双系统内核与内存管理增强平台

# 本项目的最终实现结果是
于 Quard-Star/QEMU 和 OpenSBI Domain 构建 RISC-V AMP 系统，将多核 xv6 与 FreeRTOS 分别运行于独立 HART 和内存域。完成 xv6 的 S-mode 启动、多核 HSM 启动、定时器及设备适配；参考 ChCore 在 xv6 中实现 Buddy 页面分配器和 Slab 小对象分配器；设计基于共享内存双向环形队列的 xv6-FreeRTOS 通信机制，实现实时任务控制、数据上报以及通信延迟、吞吐量和任务抖动测试。
