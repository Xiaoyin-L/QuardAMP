
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/char/serial.h"
#include "hw/char/serial-mm.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/quard_star.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "chardev/char.h"
#include "system/arch_init.h"
#include "system/device_tree.h" 
#include "system/system.h"


#define QUARD_STAR_FLASH_SECTOR_SIZE (256 * KiB)

/* RISC-V 特权规范建议将核心私有和外设控制寄存器放在低地址（即 32 位地址空间的前 2GB）
 * 启动流程简单：CPU 复位后，首先从 0x0000_0000 开始执行。
 * 将 MROM 和 SRAM 放在最低地址，CLINT/PLIC 紧随其后，
 * 构成了一个“启动代码 → 片上内存 → 核心控制 → 外设中断 → 主存”的自然层级。
 */
static const MemMapEntry virt_memmap[] = {
    [QUARD_STAR_MROM]  = {        0x0,        0x8000 },
    [QUARD_STAR_SRAM]  = {     0x8000,        0x8000 },
    [QUARD_STAR_CLINT]  = { 0x2000000,         0x10000 },
    [QUARD_STAR_PLIC]   = { 0xc000000, QUARD_STAR_PLIC_SIZE(QUARD_STAR_CPUS_MAX * 2) },
    [QUARD_STAR_UART0] = { 0x10000000,         0x100 },
    [QUARD_STAR_UART1] = { 0x10001000,         0x100 },
    [QUARD_STAR_UART2] = { 0x10002000,         0x100 },
    [QUARD_STAR_FLASH] = { 0x20000000,         0x2000000 },
    [QUARD_STAR_DRAM]  = { 0x80000000,           0x0 },
};


// 参考 hw/riscv/virt.c
#define QUARD_STAR_FLASH_SECTOR_SIZE (256 * KiB)

static PFlashCFI01 *quard_star_flash_create(RISCVVirtState *s,
                                       const char *name,
                                       const char *alias_prop_name)
{
    DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01); //创建一个 TYPE_PFLASH_CFI01 类型的 QEMU 设备对象

    // 创建一个 TYPE_PFLASH_CFI01 类型的 QEMU 设备对象
    // Flash 擦除操作的最小单位是扇区，U-Boot/Linux 需要知道这个值
    qdev_prop_set_uint64(dev, "sector-length", QUARD_STAR_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint8(dev, "width", 4); //设置 Flash 的总线宽度
    qdev_prop_set_uint8(dev, "device-width", 2); //设置 Flash 芯片的内部宽度
    qdev_prop_set_bit(dev, "big-endian", false); //设置 Flash 的字节序为小端
    qdev_prop_set_uint16(dev, "id0", 0x89);
    qdev_prop_set_uint16(dev, "id1", 0x18);
    qdev_prop_set_uint16(dev, "id2", 0x00);
    qdev_prop_set_uint16(dev, "id3", 0x00);
    qdev_prop_set_string(dev, "name", name); // 设置设备名称 "quard-star.flash0"（通过参数传入）

    
    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    // 为 Flash 设备的 drive 属性创建一个别名 
    // 用户可以通过 -drive if=pflash,file=xxx 或 -pflash xxx 挂载镜像
    object_property_add_alias(OBJECT(s), alias_prop_name,
                              OBJECT(dev), "drive");

    return PFLASH_CFI01(dev);
}

static void quard_star_flash_map(PFlashCFI01 *flash,
                            hwaddr base, hwaddr size,
                            MemoryRegion *sysmem)
{
    DeviceState *dev = DEVICE(flash);

    assert(QEMU_IS_ALIGNED(size, QUARD_STAR_FLASH_SECTOR_SIZE));
    assert(size / QUARD_STAR_FLASH_SECTOR_SIZE <= UINT32_MAX);
    qdev_prop_set_uint32(dev, "num-blocks", size / QUARD_STAR_FLASH_SECTOR_SIZE); //设置 Flash 的扇区数量
    // 实例化（Realize）Flash 设备并减少引用计数 注册 MMIO 操作（读/写/擦除）
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    // 将 Flash 的 MemoryRegion 添加到系统地址空间
    // 当 CPU 访问 base 地址时，QEMU 会路由到 Flash 设备的读写函数
    memory_region_add_subregion(sysmem, base,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev),
                                                       0));
}


static void quard_star_setup_rom_reset_vec(MachineState *machine, RISCVHartArrayState *harts,
                               hwaddr start_addr,
                               hwaddr rom_base, hwaddr rom_size,
                               uint64_t kernel_entry,
                               uint32_t fdt_load_addr)
{
    int i;
    uint32_t start_addr_hi32 = 0x00000000;

    if (!riscv_is_32bit(harts)) {
        start_addr_hi32 = start_addr >> 32;
    }
    /* reset vector */
    uint32_t reset_vec[10] = {
        0x00000297,                  /* 1:  auipc  t0, %pcrel_hi(fw_dyn) */
        0x02828613,                  /*     addi   a2, t0, %pcrel_lo(1b) */
        0xf1402573,                  /*     csrr   a0, mhartid  */
        0,
        0,
        0x00028067,                  /*     jr     t0 */
        start_addr,                  /* start: .dword */
        start_addr_hi32,
        fdt_load_addr,               /* fdt_laddr: .dword */
        0x00000000,
                                     /* fw_dyn: */
    };
    if (riscv_is_32bit(harts)) {
        reset_vec[3] = 0x0202a583;   /*     lw     a1, 32(t0) */
        reset_vec[4] = 0x0182a283;   /*     lw     t0, 24(t0) */
    } else {
        reset_vec[3] = 0x0202b583;   /*     ld     a1, 32(t0) */
        reset_vec[4] = 0x0182b283;   /*     ld     t0, 24(t0) */
    }

    /* copy in the reset vector in little_endian byte order */
    for (i = 0; i < ARRAY_SIZE(reset_vec); i++) {
        reset_vec[i] = cpu_to_le32(reset_vec[i]);
    }

    rom_add_blob_fixed_as("mrom.reset", reset_vec, sizeof(reset_vec),
                          rom_base, &address_space_memory);
}


static void quad_star_board_init(MachineState *machine)
{
    const MemMapEntry *memmap = virt_memmap; // 获取内存映射表
    RISCVVirtState *s = RISCV_VIRT_MACHINE(machine); // 获取机器状态对象
    MemoryRegion *system_memory = get_system_memory(); // 获取系统内存区域
    MemoryRegion *main_mem = g_new(MemoryRegion, 1);   // 分配 DRAM 内存区域对象
    MemoryRegion *sram_mem = g_new(MemoryRegion, 1);   // 分配 SRAM 内存区域对象
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);   // 分配 Mask ROM 内存区域对象
    
    int i, j, base_hartid, hart_count;
    char *soc_name, *plic_hart_config; //PLIC 中断配置字符串
    size_t plic_hart_config_len; // 配置字符串长度
    DeviceState *mmio_plic=NULL; //PLIC实例指针    

    /* Check socket count limit */
    if (QUARD_STAR_SOCKETS_MAX  < riscv_socket_count(machine)) {
        error_report("number of sockets/nodes should be less than %d",
            QUARD_STAR_SOCKETS_MAX );
        exit(1);
    }

    /* Initialize sockets  创建每个 Socket 的 CPU 核心 */
    for (i = 0; i < riscv_socket_count(machine); i++) {
        if (!riscv_socket_check_hartids(machine, i)) {
            error_report("discontinuous hartids in socket%d", i);
            exit(1);
        }
        // 获取该 Socket 的起始 Hart ID
        base_hartid = riscv_socket_first_hartid(machine, i);
        if (base_hartid < 0) {
            error_report("can't find hartid base for socket%d", i);
            exit(1);
        }
        // 获取该 Socket 的 Hart 数量
        hart_count = riscv_socket_hart_count(machine, i);
        if (hart_count < 0) {
            error_report("can't find hart count for socket%d", i);
            exit(1);
        }
        // 创建 SOC 对象（包含多个 Hart）
        soc_name = g_strdup_printf("soc%d", i);
        object_initialize_child(OBJECT(machine), soc_name, &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        g_free(soc_name);
         // 设置 CPU 属性
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        // 实例化硬件
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_fatal);

        // 创建 ACLINT 软件中断和定时器
        riscv_aclint_swi_create(
            memmap[QUARD_STAR_CLINT].base + i * memmap[QUARD_STAR_CLINT].size,
            base_hartid, hart_count, false);
        riscv_aclint_mtimer_create(
            memmap[QUARD_STAR_CLINT].base + i * memmap[QUARD_STAR_CLINT].size +
                RISCV_ACLINT_SWI_SIZE,
            RISCV_ACLINT_DEFAULT_MTIMER_SIZE, base_hartid, hart_count,
            RISCV_ACLINT_DEFAULT_MTIMECMP, RISCV_ACLINT_DEFAULT_MTIME,
            RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ, true);

        plic_hart_config_len =
            (strlen(QUARD_STAR_PLIC_HART_CONFIG) + 1) * hart_count;
        plic_hart_config = g_malloc0(plic_hart_config_len);
        for (j = 0; j < hart_count; j++) {
            if (j != 0) {
                strncat(plic_hart_config, ",", plic_hart_config_len);
            }
            strncat(plic_hart_config, QUARD_STAR_PLIC_HART_CONFIG,
                plic_hart_config_len);
            plic_hart_config_len -= (strlen(QUARD_STAR_PLIC_HART_CONFIG) + 1);
        }

        s->plic[i] = sifive_plic_create(
            memmap[QUARD_STAR_PLIC].base + i * memmap[QUARD_STAR_PLIC].size,
            plic_hart_config, hart_count, base_hartid,
            QUARD_STAR_PLIC_NUM_SOURCES,
            QUARD_STAR_PLIC_NUM_PRIORITIES,
            QUARD_STAR_PLIC_PRIORITY_BASE,
            QUARD_STAR_PLIC_PENDING_BASE,
            QUARD_STAR_PLIC_ENABLE_BASE,
            QUARD_STAR_PLIC_ENABLE_STRIDE,
            QUARD_STAR_PLIC_CONTEXT_BASE,
            QUARD_STAR_PLIC_CONTEXT_STRIDE,
            memmap[QUARD_STAR_PLIC].size);
        g_free(plic_hart_config);

         // 如果是第一个 Socket，则将PLIC对象保存到 mmio_plic 中
        if (i == 0) {
            mmio_plic = s->plic[i];
        }
    }

    
    /* register system main memory (actual RAM) */
    //初始化 DRAM（主内存）
    //创建一块 RAM 内存区域，大小为 machine->ram_size（由 -m 参数指定）
    memory_region_init_ram(main_mem, NULL, "riscv_quard_star_board.dram",
                           machine->ram_size, &error_fatal);
    // 将这块 RAM 挂载到系统地址空间的 QUARD_STAR_DRAM.base 位置
    memory_region_add_subregion(system_memory, memmap[QUARD_STAR_DRAM].base,
        main_mem);
    //通常用作高速缓存或启动时的临时存储，大小由 memmap 表定义。
    memory_region_init_ram(sram_mem, NULL, "riscv_quard_star_board.sram", //大小由 memmap 表定义。
                           memmap[QUARD_STAR_SRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[QUARD_STAR_SRAM].base,
        sram_mem);
    //存储启动代码（Bootloader），CPU 复位后从这里开始执行
    memory_region_init_rom(mask_rom, NULL, "riscv_quard_star_board.mrom",
                           memmap[QUARD_STAR_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[QUARD_STAR_MROM].base,
                                mask_rom);
    //在 ROM 中设置 CPU 复位后的第一条指令地址（通常为 0x1000 或 0x80000000）。这个函数会将启动代码写入 ROM，使 CPU 上电后能正确跳转到 Bootloader。
    quard_star_setup_rom_reset_vec(machine, &s->soc[0], virt_memmap[QUARD_STAR_FLASH].base,
                              virt_memmap[QUARD_STAR_MROM].base,
                              virt_memmap[QUARD_STAR_MROM].size,
                              0x0, 0x0);

    serial_mm_init(system_memory, memmap[QUARD_STAR_UART0].base,
        0, qdev_get_gpio_in(DEVICE(mmio_plic), QUARD_STAR_UART0_IRQ), 399193,
        serial_hd(0), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(system_memory, memmap[QUARD_STAR_UART1].base,
        0, qdev_get_gpio_in(DEVICE(mmio_plic), QUARD_STAR_UART1_IRQ), 399193,
        serial_hd(1), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(system_memory, memmap[QUARD_STAR_UART2].base,
        0, qdev_get_gpio_in(DEVICE(mmio_plic), QUARD_STAR_UART2_IRQ), 399193,
        serial_hd(2), DEVICE_LITTLE_ENDIAN);

    s->flash = quard_star_flash_create(s, "quard-star.flash0", "pflash0");
    pflash_cfi01_legacy_drive(s->flash, drive_get(IF_PFLASH, 0, 0));
    quard_star_flash_map(s->flash, virt_memmap[QUARD_STAR_FLASH].base,
                         virt_memmap[QUARD_STAR_FLASH].size, system_memory);
}


static void quard_star_machine_instance_init(Object *obj)
{
}

static void quard_star_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc); // 将通用对象类指针转换为 MachineClass 指针

    mc->desc = "RISC-V Quard Star board";   // 机器描述信息
    mc->init = quad_star_board_init;        // 指定机器初始化函数
    mc->max_cpus = QUARD_STAR_CPUS_MAX;     // 最大支持的 CPU 数量
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;  // 默认 CPU 类型
    mc->pci_allow_0_address = true;         // 允许 PCI 地址 0（某些设备需要）
    mc->possible_cpu_arch_ids = riscv_numa_possible_cpu_arch_ids;        // CPU 架构 ID 列表
    mc->cpu_index_to_instance_props = riscv_numa_cpu_index_to_props;     // CPU 索引转实例属性
    mc->get_default_cpu_node_id = riscv_numa_get_default_cpu_node_id;    // 获取默认 NUMA 节点 ID
    mc->numa_mem_supported = true;          // 支持 NUMA 内存配置
    
}

// 用于描述 QEMU 中一种机器类型的元数据信息
static const TypeInfo quard_star_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("quard-star"),
    .parent     = TYPE_MACHINE,
    .class_init = quard_star_machine_class_init,
    .instance_init = quard_star_machine_instance_init,
    .instance_size = sizeof(RISCVVirtState),
};

static void quard_star_machine_init_register_types(void)
{
    // 将类型注册到 QEMU 的类型系统
    type_register_static(&quard_star_machine_typeinfo);
}
// 在 QEMU 启动时自动调用注册函数
type_init(quard_star_machine_init_register_types)
