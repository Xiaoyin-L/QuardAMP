# QuardAMP PCIe 加速器阶段五设计方案

## 目标

阶段五建立性能、稳定性、恢复和 benchmark 的最小闭环。它不改变阶段四的隔离架构，而是在其上增加可重复测量和异常恢复验证。

目标：

- 性能：测量不同 DMA 长度下的耗时、吞吐和中断次数。
- 稳定性：连续多轮 queue0/domain0 DMA，验证没有 IOMMU fault、数据校验通过。
- 恢复：故意触发一次非法 IOVA fault，清理 fault 后继续合法 DMA。
- benchmark：新增用户态 `pciebench` 命令，输出可比较指标。

## 测试入口

阶段五保留阶段四自检：

```text
pcietest
```

新增性能和恢复测试：

```text
pciebench
```

`pcietest` 用于确认功能正确性，包括多队列、多 domain、MSI-X 隔离。`pciebench` 用于持续测量性能、稳定性和恢复能力。

## 内核接口

新增 syscall：

```text
pcieaccelbench(result)
```

内核 driver 填充：

```c
struct pcie_accel_bench_result {
  uint32 ok;
  uint32 sizes;
  uint32 recovery_faults;
  uint32 recovery_ok;
  uint32 vector0_count;
  uint32 vector1_count;
  struct pcie_accel_bench_entry entries[4];
};
```

每个 entry 记录：

- DMA 长度
- 迭代次数
- 总耗时 ticks
- 总字节数
- IRQ 增量

用户态负责把 ticks 换算成 us、op/s、KB/s。

## Benchmark 负载

当前测试长度：

| 长度 | 迭代次数 | 目的 |
| ---: | ---: | --- |
| 64B | 128 | 小包 MMIO/MSI-X 开销 |
| 128B | 128 | 阶段一到四默认测试长度 |
| 1024B | 64 | 中等 DMA 长度 |
| 4096B | 32 | 当前 accelerator 最大单次传输 |

DMA buffer 从 128B 扩展到 4096B，IOMMU window 从 4KiB 扩展到 16KiB，保证 src/dst bounce buffer 同时落在 domain0 window 内。

## 恢复测试

恢复测试流程：

1. queue0/domain0 提交越界 IOVA。
2. IOMMU 记录 fault，accelerator 返回 fault interrupt。
3. driver 清理 domain0 fault。
4. queue0/domain0 立即重新提交合法 DMA。
5. 校验 DMA 数据正确。

如果第 4 步或第 5 步失败，说明 fault 后恢复路径不可靠。

## 期望输出

示例：

```text
pciebench: run PCIe accelerator performance/stability benchmark
  len=64 iters=128 elapsed=...us avg=...us rate=... op/s bw=... KB/s irq=128
  len=128 iters=128 elapsed=...us avg=...us rate=... op/s bw=... KB/s irq=128
  len=1024 iters=64 elapsed=...us avg=...us rate=... op/s bw=... KB/s irq=64
  len=4096 iters=32 elapsed=...us avg=...us rate=... op/s bw=... KB/s irq=32
  recovery: faults=1 recovered=1 vectors=.../...
pciebench: passed
```

## 当前边界

- 当前 benchmark 是同步 MMIO submit + MSI-X completion，不是异步批量提交。
- 吞吐包含 MMIO 寄存器写入、IMSIC 中断和内核等待开销。
- queue1/domain1 仍由 `pcietest` 做隔离验证，`pciebench` 主要压 queue0/domain0。
- 还没有持久化性能基线，也没有失败自动重置设备。

## 后续方向

- 增加批量提交，减少每次 DMA 的 MMIO doorbell 和 MSI-X 开销。
- 加入超时后的设备 reset/reinit 流程。
- 增加长时间 soak test，比如 10k/100k 次 DMA。
- 把 benchmark 输出保存为可比较的基线。
- 后续 RTOS 真实驱动 queue1 后，加入双 domain 并发 benchmark。

## 移植和联调注意事项

### DMA buffer 和 IOMMU window 要一起扩大

阶段四只验证 128B，阶段五需要 4096B DMA。src/dst 两个 buffer 都必须落在同一个 IOMMU window 内，因此只扩大 DMA buffer 不够，还要扩大 `QACC_IOVA_SIZE`。

### 恢复测试必须按 domain 清 fault

非法 IOVA fault 后必须清理 domain0 fault，再执行合法 DMA。否则后续合法 DMA 即使数据正确，也会因为旧 fault 状态造成误判。

### benchmark 数值不是裸 PCIe 带宽

当前 QEMU 模型是同步执行，且每次传输都走 MMIO command 和 MSI-X interrupt。输出更适合作为阶段内回归基线，而不是硬件 PCIe 极限带宽。
