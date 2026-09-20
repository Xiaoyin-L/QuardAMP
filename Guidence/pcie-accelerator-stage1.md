# QuardAMP PCIe 加速器阶段一设计

## 目标

阶段一的目标是先打通一条最小可用的 PCIe 加速器路径，同时保持现有 xv6 到 FreeRTOS 的 mailbox/rpmsg 通道不变。

验证路径如下：

```text
xv6 driver -> PCIe ECAM config -> BAR0 command registers
           -> accelerator DMA read/write DDR
           -> PCIe MSI -> hart0 S-IMSIC vector 32
           -> xv6 interrupt handler -> pcietest pass
```

FreeRTOS/hart7 不参与阶段一的数据路径。

## 硬件模型

`quard-star` QEMU 机器现在包含一个通用 PCIe Root Complex：

| 区域 | Guest 地址 | 大小 | 用途 |
| --- | ---: | ---: | --- |
| ECAM | `0x30000000` | `0x10000000` | PCI 配置空间 |
| MMIO32 | `0x40000000` | `0x40000000` | PCI BAR 窗口 |
| PIO | `0x03000000` | `0x10000` | PCI I/O 窗口 |

内置 endpoint 设备为 `quardamp-accel`，位置是 bus 0、device 1、function 0：

| 字段 | 值 |
| --- | --- |
| Vendor ID | `0x1efd` |
| Device ID | `0xa001` |
| BAR0 | 64 KiB MMIO |
| 中断 | MSI vector 32 |

## BAR0 寄存器 ABI

所有寄存器均为 32-bit little-endian。

| 偏移 | 名称 | 方向 | 含义 |
| ---: | --- | --- | --- |
| `0x00` | `MAGIC` | R | `0x51414343` (`QACC`) |
| `0x04` | `STATUS` | R/W1C | bit0 done，bit1 busy |
| `0x08` | `SRC_LO` | R/W | DMA 源地址低 32 位 |
| `0x0c` | `SRC_HI` | R/W | DMA 源地址高 32 位 |
| `0x10` | `DST_LO` | R/W | DMA 目标地址低 32 位 |
| `0x14` | `DST_HI` | R/W | DMA 目标地址高 32 位 |
| `0x18` | `LEN` | R/W | 传输长度，最大 4096 |
| `0x1c` | `CMD` | W | bit0 run，bit1 完成后触发中断 |
| `0x20` | `IRQ_STATUS` | R | bit0 done interrupt pending |
| `0x24` | `IRQ_ACK` | W1C | 写 bit0 清除中断 |

加速器从 `SRC` 读取 `LEN` 字节，将每个字节与 `0x5a` 做 XOR，然后把结果写入 `DST`。传输完成后设置 done 状态，并在请求中断时触发 MSI。

## xv6 驱动

xv6 驱动当前刻意保持最小化：

1. 扫描 ECAM bus 0，查找 vendor/device `0x1efd:0xa001`。
2. 将 BAR0 分配到固定地址 `0x40000000`。
3. 使能 PCI memory space 和 bus mastering。
4. 查找 MSI capability。
5. 配置 MSI address `0x28000000`、data `32`，目标为 hart0 S-IMSIC。
6. 在 hart0 上使能 IMSIC EIE bit 32。
7. 暴露 `pcietest`，用于运行一次 DMA/MSI 自测试。

这还不是通用 PCI bus manager，而是面向单个已知 endpoint 的 bring-up 驱动。

## 为什么暂不加入 IOMMU

阶段一有意使用物理地址 DMA。这样第一个 PCIe 里程碑可以聚焦在：

- ECAM 枚举
- BAR 映射
- 设备 bus mastering
- DMA 访问 DDR
- MSI 投递到 IMSIC

IOMMU 支持放到后续阶段，在基础 DMA/MSI 路径稳定之后再引入。

## 与 mailbox/rpmsg 的关系

mailbox/rpmsg 通道仍然保留，但阶段一不扩展它。

当前职责划分如下：

```text
xv6 <-> PCIe accelerator:
  PCIe BAR + DMA buffer + MSI

xv6 <-> FreeRTOS:
  existing shared memory rpmsg + mailbox doorbell
```

后续阶段可以让 FreeRTOS 通过 rpmsg 请求加速器任务，由 xv6 作为 PCIe owner 负责真正提交 work。

## 测试

启动 QuardAMP 后运行：

```sh
pcietest
```

期望输出：

```text
pcietest: run PCIe accelerator DMA/MSI selftest
pcie-accel: selftest ok len=128 irq_count=1
pcietest: passed
```
