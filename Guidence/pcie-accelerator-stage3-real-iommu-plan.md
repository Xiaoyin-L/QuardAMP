# QuardAMP PCIe 加速器正式阶段三设计方案

## 目标

正式阶段三把当前 endpoint 内部 IOVA shim 迁移为独立 IOMMU / DMA protection block。

目标拓扑是：

```text
PCIe Accelerator
      |
      | DMA request with IOVA
      v
PCIe Root Complex / QuardAMP IOMMU
      |
      | translated PA
      v
DDR
```

阶段三完成后，accelerator 不再接收裸物理地址。xv6 负责为设备建立 IOVA 映射，设备 DMA 前必须经过独立 IOMMU block 的授权检查和 IOVA -> PA 翻译。未授权 IOVA 必须 fault。

## 为什么重做阶段三

当前 `pcie-accelerator-stage3.md` 是 prototype：

- IOMMU 逻辑在 `quardamp-accel` endpoint 内部。
- 映射寄存器挂在 accelerator BAR0 中。
- 它验证了 IOVA/fault 软件流程，但没有体现真实系统里 IOMMU 位于设备和 DDR 之间的结构。

正式阶段三改成独立 MMIO block：

```text
xv6 IOMMU driver -> QuardAMP IOMMU MMIO
accelerator BAR  -> 只提交 IOVA
accelerator DMA  -> 调用独立 IOMMU 翻译
```

这样阶段四扩展多 domain、多队列、MSI-X 时，权限边界不会被绑死在单个 endpoint 内部。

## 最小硬件模型

先实现一个 QuardAMP-specific IOMMU block，不一次性实现完整 RISC-V IOMMU 1.0。

| 项目 | 设计 |
| --- | --- |
| 设备形态 | SoC MMIO block |
| MMIO base | `0x10005000` |
| MMIO size | `0x1000` |
| 映射能力 | 单个连续 IOVA window |
| 权限 | read / write |
| fault | 记录 fault IOVA 和 fault reason |
| 中断 | 阶段三暂由 accelerator MSI 通知完成/fault；后续可加独立 fault IRQ |

最小寄存器：

| 偏移 | 名称 | 方向 | 含义 |
| ---: | --- | --- | --- |
| `0x00` | `CTRL` | R/W | bit0 enable |
| `0x04` | `STATUS` | R/W1C | bit0 fault |
| `0x08` | `IOVA_LO` | R/W | IOVA base 低 32 位 |
| `0x0c` | `IOVA_HI` | R/W | IOVA base 高 32 位 |
| `0x10` | `PA_LO` | R/W | PA base 低 32 位 |
| `0x14` | `PA_HI` | R/W | PA base 高 32 位 |
| `0x18` | `MAP_LEN` | R/W | window 长度 |
| `0x1c` | `MAP_PERM` | R/W | bit0 read，bit1 write |
| `0x20` | `FAULT_LO` | R | fault IOVA 低 32 位 |
| `0x24` | `FAULT_HI` | R | fault IOVA 高 32 位 |

翻译规则：

```text
if !enable:
  IOVA 被当作 PA 直通，仅用于兼容阶段一/二

if enable:
  检查 IOVA range
  检查 read/write permission
  PA = PA_BASE + (IOVA - IOVA_BASE)
```

## xv6 软件模型

xv6 新增最小 IOMMU driver：

```text
iommu_init()
iommu_map_window(iova, pa, len, perm)
iommu_enable()
iommu_fault_addr()
iommu_clear_fault()
```

PCIe accelerator driver 不再写 BAR0 内部 IOMMU 寄存器，而是：

1. 从阶段二 DMA carveout 分配 bounce buffer。
2. 调用 IOMMU driver 建立 IOVA window。
3. BAR0 command 中写 `src_iova/dst_iova`。
4. 等待 MSI。
5. 检查 IOMMU fault 状态。

## 验证口径

沿用 `pcietest`：

1. 合法 IOVA：
   - `src_iova/dst_iova` 位于授权 window 内。
   - DMA 结果正确。
   - 无 IOMMU fault。

2. 非法 IOVA：
   - 提交 `bad_iova = iova_base + iova_size + 64`。
   - IOMMU 记录 fault。
   - accelerator 不执行非法 DMA。
   - xv6 看到 fault 地址等于 `bad_iova`。

期望输出保持：

```text
pcie-accel: selftest ok len=128 irq_count=2 bounce=on iommu=on fault=blocked
pcietest: passed
```

## 移植和联调问题记录

本节记录阶段三从 endpoint 内部 prototype 迁移到独立 IOMMU block 时遇到的问题，作为后续定位同类问题的参考。

### 问题一：原阶段三不是独立 IOMMU

现象：

- `pcie-accelerator-stage3.md` 中的 IOMMU 寄存器挂在 accelerator BAR0。
- accelerator 内部自己完成 IOVA 检查和 PA 翻译。
- 从软件测试看可以得到 `iommu=on fault=blocked`，但硬件拓扑上仍然不是真正位于设备和 DDR 之间的 IOMMU。

原因：

- 第一版阶段三为了快速验证 IOVA/fault 语义，把 IOMMU shim 放进了 endpoint。
- 这能验证驱动流程，但不能表达 Root Complex / SoC 侧 DMA protection block。

解决：

- 将旧文档标记为 prototype。
- 新增独立 `quardamp-iommu` QEMU SysBus MMIO 设备，挂在 `0x10005000`。
- accelerator DMA 路径改为调用独立 IOMMU 翻译接口。
- xv6 PCIe driver 不再写 accelerator BAR0 的 IOMMU 寄存器，而是通过 `iommu.c` 配置独立 IOMMU。

### 问题二：QEMU 新设备编译缺少错误接口头文件

现象：

- 第一次编译 QEMU 时，`quardamp_iommu.c` 在 `error_setg()` 相关路径报错。

原因：

- QEMU 的 `error_setg()` 需要包含 `qapi/error.h`。
- 新增设备文件只包含了基础 QEMU 头文件和设备头文件，缺少该声明。

解决：

- 在 `qemu-10.2.4/hw/misc/quardamp_iommu.c` 中补充：

```c
#include "qapi/error.h"
```

### 问题三：IOMMU MMIO 需要同时加入 QEMU board 和 xv6 映射

现象：

- 独立 IOMMU 设备即使在 QEMU 中创建，xv6 进入分页后如果没有映射 `0x10005000`，后续访问会异常。

原因：

- xv6 的 kernel page table 需要显式映射 MMIO 区域。
- board 侧创建设备和 guest 侧映射 MMIO 是两件事，缺一不可。

解决：

- QEMU `quard-star` memmap 中加入 `QUARD_STAR_IOMMU = 0x10005000`。
- board init 中通过 `sysbus_create_simple(TYPE_QUARDAMP_IOMMU, 0x10005000, NULL)` 创建设备。
- xv6 `memlayout.h` 增加：

```c
#define QUARD_IOMMU      0x10005000L
#define QUARD_IOMMU_SIZE 0x1000
```

- xv6 `vm.c` 中增加 `kvmmap()`，保证分页开启后仍可访问 IOMMU MMIO。

### 问题四：IOMMU 初始化时序要早于 PCIe accelerator 初始化

现象：

- accelerator driver 初始化时需要立即配置 IOVA window。
- 如果 IOMMU 尚未初始化，后续 DMA 请求可能走 passthrough 或保留旧 fault 状态。

原因：

- 阶段三的 accelerator 已经只提交 IOVA，不再提交裸 PA。
- 因此 IOMMU 必须在 PCIe accelerator 自检或用户态 `pcietest` 前完成 disable、map、clear fault、enable。

解决：

- xv6 `main.c` 中在 `dma_init()` 后调用 `iommu_init()`。
- PCIe accelerator driver 在每次配置 window 时执行：

```text
iommu_disable()
iommu_map_window(...)
iommu_clear_fault()
iommu_enable()
```

### 问题五：QEMU install 目标耗时较长

现象：

- `make -C qemu-10.2.4/build install` 会继续编译并安装一批 QEMU 工具、qtest 和 BIOS 资源。
- 日志里会出现大量与本次 IOMMU 修改无关的测试目标。

原因：

- 当前 build 配置下，install 目标会确保相关 target 都构建完成后再安装到 `output/qemu`。

解决：

- 判断本次设备代码是否能编过时，可以先运行：

```text
make -C qemu-10.2.4/build qemu-system-riscv64
```

- 只有需要刷新 `output/qemu/bin/qemu-system-riscv64` 时，再运行 install。

### 问题六：旧 BAR0 IOMMU prototype 寄存器仍有兼容残留

现象：

- `quardamp_accel.c` 中还能看到旧的 `QACC_REG_IOMMU_*` / `QACC_IOMMU_*` 定义。
- 正式阶段三运行时，xv6 不再通过这些 BAR0 寄存器配置 IOMMU。

原因：

- 为了降低重做阶段三的风险，本次先把 DMA 翻译路径迁移到独立 IOMMU，并保留旧寄存器定义作为 prototype 兼容残留。

解决：

- 当前功能验证不依赖这些旧寄存器。
- 后续清理阶段可以删除 endpoint 内部 IOMMU 配置寄存器，只保留必要的 fault 镜像状态或调试字段。

### 问题七：验证时要区分 UART 输出和 xv6 shell 输出

现象：

- 之前 `slabtest`、`icctest` 调试时，UART2 未必有可见变化。
- 阶段三验证的关键输出在 xv6 shell 所在串口，而不是 RTOS UART2。

原因：

- 本阶段验证路径是 xv6 用户态 `pcietest` -> xv6 PCIe driver -> PCIe accelerator -> MSI/IMSIC。
- RTOS UART2 不参与 `pcietest` 的输出路径。

解决：

- 阶段三验证以 xv6 shell 中的以下输出为准：

```text
iommu: mmio=0x10005000
pcie-accel: selftest ok len=128 irq_count=2 bounce=on iommu=on fault=blocked
pcietest: passed
```

- UART2 日志只用于 RTOS 侧功能或跨核 IPC 调试，不作为 PCIe/IOMMU 阶段三的通过标准。

## 后续迁移到标准 RISC-V IOMMU

这个正式阶段三仍然是 QuardAMP-specific 最小模型，不是最终标准设备。它的价值是先把独立 IOMMU block、xv6 IOMMU driver、IOVA/fault 路径拆清楚。

后续可以替换为：

- QEMU `riscv-iommu-pci`
- QEMU `riscv-iommu-sys`
- RISC-V IOMMU 1.0 page table
- device context / process context
- IOTLB flush
- fault queue / fault interrupt

阶段四再在这个基础上扩展多 domain、多队列和 MSI-X vector 分配。
