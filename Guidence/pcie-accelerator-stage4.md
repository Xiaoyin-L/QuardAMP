# QuardAMP PCIe 加速器阶段四设计方案

## 目标

阶段四在阶段三独立 IOMMU 的基础上加入最小可验证的隔离维度：

- 多 domain：xv6 和 RTOS/后续端点使用不同 DMA 授权域。
- 多队列：accelerator 暴露多个命令队列，每个队列绑定一个 domain。
- MSI-X vector 分配：每个队列绑定独立 MSI-X vector，并投递到不同 IMSIC interrupt ID。
- 隔离验证：一个 domain 的 IOVA 授权不能被另一个 queue/domain 复用。

本阶段仍然是 QuardAMP-specific prototype，不是完整 PCIe SR-IOV、PASID 或标准 RISC-V IOMMU 1.0。

## 拓扑

```text
                 QuardAMP SoC
┌─────────────────────────────────────────────────┐
│ Hart0~6 / xv6                                   │
│   queue0 -> domain0 -> MSI-X vector0 -> IMSIC32 │
│                                                 │
│ Hart7 / FreeRTOS                                │
│   queue1 -> domain1 -> MSI-X vector1 -> IMSIC33 │
│                                                 │
│ QuardAMP IOMMU                                  │
│   domain0: xv6 DMA window                       │
│   domain1: RTOS/预留 DMA window                 │
│                                                 │
│ PCIe Root Complex -> PCIe Accelerator           │
└─────────────────────────────────────────────────┘
```

当前 xv6 只实际驱动 queue0，queue1 作为 RTOS/后续隔离入口，先由 `pcietest` 做负向隔离验证。

## accelerator BAR0 扩展

旧阶段一到三的寄存器继续保留，默认操作 queue0。阶段四新增队列选择和队列属性寄存器：

| 偏移 | 名称 | 含义 |
| ---: | --- | --- |
| `0x50` | `QUEUE_SEL` | 选择当前 BAR0 命令寄存器操作的 queue |
| `0x54` | `QUEUE_DOMAIN` | 当前 queue 绑定的 IOMMU domain |
| `0x58` | `QUEUE_VECTOR` | 当前 queue 绑定的 MSI-X vector |
| `0x5c` | `QUEUE_COUNT` | accelerator 支持的 queue 数 |

当前实现：

| queue | domain | MSI-X vector | IMSIC IRQ | 用途 |
| ---: | ---: | ---: | ---: | --- |
| 0 | 0 | 0 | 32 | xv6 accelerator DMA |
| 1 | 1 | 1 | 33 | RTOS/后续预留，阶段四做隔离验证 |

`STATUS/SRC/DST/LEN/CMD` 这些旧寄存器根据 `QUEUE_SEL` 指向不同 queue。这样旧 driver 默认不写 `QUEUE_SEL` 时仍然落在 queue0。

## IOMMU domain 扩展

阶段三的 IOMMU 是单 window。阶段四扩为 2 个 domain，每个 domain 有独立的：

- `STATUS`
- `IOVA_BASE`
- `PA_BASE`
- `MAP_LEN`
- `MAP_PERM`
- `FAULT_ADDR`

新增寄存器：

| 偏移 | 名称 | 含义 |
| ---: | --- | --- |
| `0x28` | `DOMAIN_SEL` | 选择后续 IOMMU window/fault 寄存器访问哪个 domain |

xv6 新增 domain-aware API：

```text
iommu_select_domain(domain)
iommu_map_domain_window(domain, iova, pa, len, perm)
iommu_domain_status(domain)
iommu_domain_fault_addr(domain)
iommu_clear_domain_fault(domain)
```

旧 API 保留并默认访问 domain0。

## MSI-X 分配

QEMU accelerator 新增 MSI-X capability，使用独立 BAR4 保存 MSI-X table/PBA。xv6 初始化时：

1. 配置 BAR0 到 `0x40000000`。
2. 配置 MSI-X BAR4 到 `0x40010000`。
3. 在 MSI-X table 中写入两个 vector：
   - vector0 data = `32`
   - vector1 data = `33`
4. 打开 MSI-X enable，关闭 function mask。
5. IMSIC 侧打开 IRQ32 和 IRQ33。

为了兼容，QEMU 设备仍保留旧 MSI capability。xv6 优先启用 MSI-X；如果找不到 MSI-X，才回退旧 MSI。

## 验证路径

`pcietest` 阶段四验证三件事：

1. queue0/domain0 合法 DMA：
   - IOVA 在 domain0 授权 window 内。
   - DMA 结果正确。
   - vector0 / IRQ32 收到完成中断。

2. queue0/domain0 越界 fault：
   - 提交 `bad_iova = iova_base + iova_size + 64`。
   - domain0 记录 fault。
   - vector0 / IRQ32 收到 fault 中断。

3. queue1/domain1 隔离 fault：
   - queue1 绑定 domain1 和 vector1。
   - domain1 不映射 xv6 的 IOVA window。
   - 即使 queue1 使用 queue0 的 `src_iova/dst_iova`，也必须 fault。
   - domain0 不应出现 fault。
   - vector1 / IRQ33 收到 fault 中断。

期望输出：

```text
iommu: mmio=0x10005000 domains=2
pcie-accel: ... vectors=32/33 queues=2 domains=2 ...
pcie-accel: selftest ok len=128 irq_count=3 vectors=2/1 queues=2 domains=2 bounce=on iommu=on fault=blocked isolation=on
pcietest: passed
```

`irq_count=3` 是预期现象：

- queue0 合法 DMA 完成一次。
- queue0 非法 IOVA fault 一次。
- queue1/domain1 隔离 fault 一次。

## 当前边界

- 队列执行仍然是同步 MMIO command，不是后台异步硬件队列。
- queue1 尚未由 RTOS driver 真实驱动，当前只作为隔离测试对象。
- domain 仍是单连续 window，不是页表。
- MSI-X 已有多 vector 分配，但还没有做动态 vector allocator。

这些边界是有意保留的：阶段四先验证权限和中断隔离的结构，后续再扩展真正的共享队列、RTOS driver、动态 vector 管理和标准 IOMMU。

## 移植和联调注意事项

### MSI-X BAR 必须映射到 xv6

MSI-X table 在 BAR4。xv6 必须把 PCIe MMIO 映射范围扩大到覆盖 `0x40010000`，否则写 MSI-X table 会触发访问异常。

### MSI-X table 和 capability 要匹配

当前 QEMU 使用 `msix_init_exclusive_bar(..., bar=4)`，因此 xv6 要求 MSI-X table 的 BIR 为 4、offset 为 0。如果后续改 table 布局，xv6 的检查和 table 写入地址也要同步调整。

### domain fault 状态要按 domain 清理

阶段四以后不能只调用旧的 `iommu_clear_fault()`。多 domain 测试前应清理对应 domain，否则旧 fault 可能造成误判。

### 中断计数要按 vector 看

只看总 `irq_count` 不足以证明 MSI-X 隔离。阶段四输出 `vectors=2/1`，表示 vector0 收到 queue0 的完成和 fault，vector1 收到 queue1/domain1 的 fault。

### QEMU MSI-X vector 需要显式标记 in-use

现象：

- accelerator 已经执行完成，BAR0 中可见 `status=1 irq_status=1`。
- xv6 等待中断超时，`pcietest` 输出：

```text
pcie-accel: timeout status=1 irq_status=1
pcietest: failed
```

原因：

- QEMU `msix_notify()` 依赖 vector 的使用状态。
- 只调用 `msix_init_exclusive_bar()` 和 guest 侧写 MSI-X table 还不够，设备模型需要对将要使用的 vector 调用 `msix_vector_use()`。

解决：

- 在 accelerator realize 阶段完成 MSI-X 初始化后，对两个 vector 标记使用：

```c
for (uint32_t i = 0; i < QACC_QUEUE_COUNT; i++) {
    msix_vector_use(pdev, i);
}
```

修复后 `pcietest` 可以收到 IRQ32/IRQ33，输出 `vectors=2/1`。
