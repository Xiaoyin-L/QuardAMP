# QuardAMP PCIe 加速器阶段三 Prototype 设计

> 注意：本文记录的是阶段三的 IOVA prototype。它把 IOMMU 语义临时放在 `quardamp-accel` endpoint 内部，用于验证 IOVA/fault 软件路径；它不是最终正式阶段三，也不是标准 RISC-V IOMMU 或独立 RC-side IOMMU block。正式阶段三见 `pcie-accelerator-stage3-real-iommu-plan.md`。

## 目标

阶段三在阶段二 DMA buffer 管理基础上加入 IOMMU/IOVA 授权边界。

阶段二已经保证驱动只把固定 carveout 中的 bounce buffer 交给设备，但设备看到的仍然是裸物理地址。阶段三改为：

```text
CPU 普通缓冲区
  -> DMA bounce buffer PA
  -> IOMMU 映射成授权 IOVA
  -> accelerator BAR command 使用 IOVA
  -> endpoint IOMMU 翻译/检查
  -> 合法 DMA 访问 DDR，非法 DMA fault
```

阶段三的核心验证点不是性能，而是权限语义：

- 设备不能再直接使用任意物理地址。
- BAR 命令寄存器中提交的是 IOVA。
- 只有被映射并带有对应权限的 IOVA 范围可以 DMA。
- 越界 IOVA 会触发 IOMMU fault，不执行 DMA。

## 当前实现边界

本阶段实现的是 QuardAMP 最小 IOMMU shim，位于 `quardamp-accel` endpoint 内部。它不是完整 RISC-V IOMMU 1.0 设备，也不是最终形态。

这样做的原因是：

- xv6 目前没有通用 PCI/IOMMU 子系统。
- 阶段三需要先把 IOVA/权限模型跑通，而不是一次性实现完整规范驱动。
- QEMU 已有 `riscv-iommu-pci`/`riscv-iommu-sys`，后续可以在这个模型稳定后替换为标准设备。

因此，阶段三先验证“设备 DMA 必须通过授权 IOVA”这个核心语义。

## IOMMU 寄存器扩展

阶段三在 BAR0 中追加一组 IOMMU 映射寄存器。当前只支持一个连续 IOVA window，足够覆盖阶段二分配出的 `src_dma/dst_dma` bounce buffer。

| 偏移 | 名称 | 方向 | 含义 |
| ---: | --- | --- | --- |
| `0x28` | `IOMMU_CTRL` | R/W | bit0 enable |
| `0x2c` | `IOMMU_STATUS` | R/W1C | bit0 fault |
| `0x30` | `IOVA_LO` | R/W | 授权 IOVA base 低 32 位 |
| `0x34` | `IOVA_HI` | R/W | 授权 IOVA base 高 32 位 |
| `0x38` | `PA_LO` | R/W | 映射到的物理 base 低 32 位 |
| `0x3c` | `PA_HI` | R/W | 映射到的物理 base 高 32 位 |
| `0x40` | `MAP_LEN` | R/W | 授权窗口长度 |
| `0x44` | `MAP_PERM` | R/W | bit0 read，bit1 write |
| `0x48` | `FAULT_LO` | R | fault IOVA 低 32 位 |
| `0x4c` | `FAULT_HI` | R | fault IOVA 高 32 位 |

启用 `IOMMU_CTRL.enable` 后，设备执行 DMA 前会做：

```text
IOVA + LEN 是否落在 [IOVA_BASE, IOVA_BASE + MAP_LEN)
DMA 方向是否满足 MAP_PERM
PA = PA_BASE + (IOVA - IOVA_BASE)
```

如果检查失败，设备设置：

```text
STATUS.fault = 1
IOMMU_STATUS.fault = 1
FAULT_ADDR = offending IOVA
```

并在请求中断时通过 MSI 通知 xv6。

## xv6 映射策略

xv6 当前使用一个固定 IOVA window：

| 项目 | 值 |
| --- | --- |
| IOVA base | `0x10000000` |
| IOVA size | `0x1000` |
| 权限 | read + write |

阶段二的 `src_dma/dst_dma` 来自同一个 carveout bump allocator，地址连续。xv6 将 `src_dma.pa` 作为 `PA_BASE`，映射一个 4 KiB IOVA window：

```text
src_iova = 0x10000000
dst_iova = 0x10000000 + (dst_dma.pa - src_dma.pa)
```

之后 BAR0 命令寄存器写入的是 `src_iova/dst_iova`，不再写入 `src_dma.pa/dst_dma.pa`。

## 测试路径

`pcietest` 当前包含两个验证：

1. 合法 DMA：
   - `src_iova/dst_iova` 均落在授权 window 内。
   - accelerator 完成 XOR DMA。
   - MSI 返回 xv6。
   - 结果从 bounce buffer 拷回 CPU buffer 后校验通过。

2. 非法 DMA：
   - xv6 故意提交 `bad_iova = iova_base + iova_size + 64`。
   - endpoint IOMMU 检查失败。
   - 设备设置 fault，并通过 MSI 通知。
   - xv6 校验 fault 地址等于 `bad_iova`。

期望输出：

```text
pcie-accel: bdf=0:1.0 bar0=0x40000000 msi=32 dma_src=0x89f00000 dma_dst=0x89f00080 iova=[0x10000000,0x10001000)
pcietest: run PCIe accelerator DMA/MSI selftest
pcie-accel: selftest ok len=128 irq_count=2 bounce=on iommu=on fault=blocked
pcietest: passed
```

`irq_count=2` 是预期现象：一次来自合法 DMA 完成，另一次来自非法 IOVA fault。

## 与后续阶段的关系

阶段三仍然是单设备、单 window、单队列模型。它解决的是“DMA 地址必须授权”的边界，不解决多租户隔离。

阶段四应继续扩展：

- 多 domain：xv6、RTOS、accelerator 不共享同一个 DMA 权限空间。
- 多队列：不同队列绑定不同 IOVA/权限。
- MSI-X vector 分配：不同队列或 domain 使用不同中断 vector。
- 最终替换为标准 RISC-V IOMMU 设备和更通用的 PCI/IOMMU 管理层。
