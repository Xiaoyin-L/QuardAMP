# QuardAMP PCIe 加速器阶段二设计

## 目标

阶段二在阶段一已经跑通的 PCIe BAR + DMA + MSI 路径上，补齐 DMA buffer 管理规则。

阶段二仍然不引入 IOMMU，也不改变 RTOS/rpmsg 路径。核心目标是让设备 DMA 访问的内存范围、buffer 生命周期和 cache 同步点变得明确：

```text
CPU 普通缓冲区
  -> bounce copy
  -> 固定 DMA carveout
  -> PCIe accelerator DMA read/write
  -> MSI 通知完成
  -> cache sync
  -> bounce copy 回 CPU 普通缓冲区
```

## 阶段划分

当前 PCIe 加速器路线按以下阶段推进：

| 阶段 | 内容 | 目标 |
| --- | --- | --- |
| 阶段 1 | 无 IOMMU | 跑通 PCIe BAR + DMA + MSI/MSI-X + IMSIC |
| 阶段 2 | DMA buffer 管理 | 固定 carveout / bounce buffer / cache flush 规则清楚 |
| 阶段 3 | IOMMU | 设备 DMA 只能访问被授权的 IOVA 区域 |
| 阶段 4 | 多 domain / 多队列 / MSI-X vector 分配 | xv6、RTOS、accelerator 的权限和中断都可隔离 |

## 固定 DMA Carveout

xv6 管理的 RAM 顶部预留 1 MiB 作为 DMA carveout：

| 项目 | 值 |
| --- | --- |
| `RAMTOP` | `KERNBASE + 128 MiB` |
| `DMA_CARVEOUT_SIZE` | `1 MiB` |
| `DMA_CARVEOUT_BASE` | `RAMTOP - DMA_CARVEOUT_SIZE` |
| `DMA_CARVEOUT_END` | `RAMTOP` |
| `PHYSTOP` | `DMA_CARVEOUT_BASE` |

由于 `PHYSTOP` 下移，普通页分配器不会把 carveout 区域分配给进程、文件系统缓存或内核通用对象。PCIe 设备 DMA 只使用 `kernel/dma.c` 从 carveout 中分配出来的 buffer。

## DMA Buffer 管理接口

阶段二新增一个最小 DMA arena：

| 接口 | 作用 |
| --- | --- |
| `dma_init()` | 初始化固定 carveout arena |
| `dma_alloc()` | 从 carveout 中按对齐要求分配 DMA buffer |
| `dma_bounce_to_device()` | 把 CPU 普通缓冲区复制到 DMA buffer，并执行 device 方向同步 |
| `dma_bounce_from_device()` | 执行 CPU 方向同步，并把 DMA buffer 内容复制回 CPU 普通缓冲区 |
| `dma_sync_for_device()` | 设备读 buffer 前的同步点 |
| `dma_sync_for_cpu()` | 设备写 buffer 后的同步点 |

当前实现是 bump allocator，不支持释放。阶段二的验证路径只需要长期持有少量固定 DMA buffer，这比通用 allocator 更容易审计。

## Bounce Buffer 规则

阶段二不再把任意 kernel buffer 直接交给 PCIe 设备，而是采用 bounce buffer：

1. CPU 在普通缓冲区中准备输入数据。
2. 驱动调用 `dma_bounce_to_device()`，把输入复制到 carveout 中的 `src_dma`。
3. 驱动把 `src_dma.pa` 和 `dst_dma.pa` 写入 PCIe BAR0 命令寄存器。
4. 加速器作为 PCIe bus master 只访问 carveout 中的 `src_dma/dst_dma`。
5. MSI 到达后，驱动调用 `dma_bounce_from_device()`，把 `dst_dma` 复制回 CPU 普通缓冲区。
6. CPU 只校验普通缓冲区，不直接依赖设备写过的 cache 状态。

这个规则为阶段三 IOMMU 做准备：后续只需要把 carveout buffer 映射成授权 IOVA，而不是允许设备访问任意物理地址。

## Cache 同步规则

阶段二定义同步点，即使当前 QEMU/TCG 下没有真实 cache 一致性问题，也不省略这些调用。

| 场景 | 调用 | 语义 |
| --- | --- | --- |
| CPU 写入、设备读取前 | `dma_sync_for_device()` | CPU 写入对设备可见 |
| 设备写入、CPU 读取前 | `dma_sync_for_cpu()` | 设备写入对 CPU 可见 |

当前 xv6/QEMU 实现中，这两个接口使用 memory barrier 和 RISC-V `fence rw, rw`。如果后续迁移到非一致性 cache 或真实硬件，应在这两个接口中替换为对应的 cache clean / invalidate 操作。

## 当前验证路径

`pcietest` 仍然是阶段二的验证入口，但内部路径已经从阶段一的“静态数组直接 DMA”变为：

```text
cpu_src[] -> src_dma carveout buffer
          -> PCIe accelerator DMA
          -> dst_dma carveout buffer
          -> cpu_dst[]
```

期望输出中会显示 carveout 分配出来的 DMA buffer 地址：

```text
dma: carveout [0x89f00000, 0x8a000000) size=1048576
pcie-accel: bdf=0:1.0 bar0=0x40000000 msi=32 dma_src=0x89f00000 dma_dst=0x89f00080
pcietest: run PCIe accelerator DMA/MSI selftest
pcie-accel: selftest ok len=128 irq_count=1 bounce=on
pcietest: passed
```

## 与 IOMMU 的关系

阶段二仍然没有 DMA 权限隔离。设备理论上仍可发起任意物理地址 DMA，只是 xv6 驱动不再主动把任意地址交给设备。

真正的权限边界放到阶段三：

- 为设备建立 IOVA 地址空间。
- 只把 carveout 或按需 DMA buffer 映射进 IOMMU。
- BAR 命令寄存器中写入 IOVA，而不是裸物理地址。
- 未授权 DMA 访问应被 IOMMU 拦截。
