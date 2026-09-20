#ifndef HW_RISCV_QUARD_STAR__H
#define HW_RISCV_QUARD_STAR__H

#include "hw/riscv/riscv_hart.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#include "hw/block/flash.h"
#include "hw/pci-host/gpex.h"

#define QUARD_STAR_CPUS_MAX 8
#define QUARD_STAR_SOCKETS_MAX 8

#define TYPE_RISCV_QUARD_STAR_MACHINE MACHINE_TYPE_NAME("quard-star")
typedef struct RISCVVirtState RISCVVirtState;
DECLARE_INSTANCE_CHECKER(RISCVVirtState, RISCV_VIRT_MACHINE,
                         TYPE_RISCV_QUARD_STAR_MACHINE)

struct RISCVVirtState {
    /*< private >*/
    MachineState parent;

    /*< public >*/
    RISCVHartArrayState soc[QUARD_STAR_SOCKETS_MAX];
    DeviceState *irqchip[QUARD_STAR_SOCKETS_MAX];
    GPEXHost *gpex_host;
    PFlashCFI01 *flash;
};

enum {
    QUARD_STAR_MROM,
    QUARD_STAR_SRAM,
    QUARD_STAR_CLINT,
    QUARD_STAR_APLIC_M,
    QUARD_STAR_APLIC_S,
    QUARD_STAR_IMSIC_M,
    QUARD_STAR_IMSIC_S,
    QUARD_STAR_PCIE_PIO,
    QUARD_STAR_UART0,
    QUARD_STAR_UART1,
    QUARD_STAR_UART2,
    QUARD_STAR_VIRTIO,   // 新增：VirtIO 设备的 memmap 索引
    QUARD_STAR_MAILBOX,  // 新增：AMP mailbox 设备的 memmap 索引
    QUARD_STAR_IOMMU,
    QUARD_STAR_FLASH,
    QUARD_STAR_PCIE_ECAM,
    QUARD_STAR_PCIE_MMIO,
    QUARD_STAR_DRAM,
};

enum {
    QUARD_STAR_UART0_IRQ = 10,
    QUARD_STAR_UART1_IRQ = 11,
    QUARD_STAR_UART2_IRQ = 12,
    QUARD_STAR_VIRTIO_IRQ = 1,  // 新增：与 xv6 的 VIRTIO0_IRQ 保持一致
    /*
     * AMP mailbox 双向 doorbell 使用两条独立 PLIC 中断源：
     *   TO_RTOS: xv6 写 mailbox 后通知 FreeRTOS(hart7)
     *   TO_XV6 : FreeRTOS 写 mailbox 后通知 xv6(hart0~6)
     * 分成两源避免单源被 PLIC 仲裁到单一 context。
     */
    QUARD_STAR_MAILBOX_TO_RTOS_IRQ = 13,
    QUARD_STAR_MAILBOX_TO_XV6_IRQ  = 14,
    QUARD_STAR_PCIE_IRQ = 32,
};

#define QUARD_STAR_IRQCHIP_NUM_SOURCES 127
#define QUARD_STAR_IRQCHIP_NUM_MSIS    255
#define QUARD_STAR_IRQCHIP_NUM_PRIO_BITS 3
#define QUARD_STAR_IMSIC_GROUP_MAX_SIZE (1UL << 24)

#endif
