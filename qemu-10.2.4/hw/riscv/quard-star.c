
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
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/quard_star.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "chardev/char.h"
#include "system/arch_init.h"
#include "system/device_tree.h" 
#include "system/system.h"


static const MemMapEntry virt_memmap[] = {
    [QUARD_STAR_MROM]  = {        0x0,        0x8000 },
    [QUARD_STAR_SRAM]  = {     0x8000,        0x8000 },
    [QUARD_STAR_UART0] = { 0x10000000,         0x100 },
    [QUARD_STAR_DRAM]  = { 0x80000000,           0x0 },
};


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
    RISVCVirState *s = SPIKE_MACHINE(machine); // 获取机器状态对象
    MemoryRegion *system_memory = get_system_memory(); // 获取系统内存区域
    MemoryRegion *main_mem = g_new(MemoryRegion, 1);   // 分配 DRAM 内存区域对象
    MemoryRegion *sram_mem = g_new(MemoryRegion, 1);   // 分配 SRAM 内存区域对象
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);   // 分配 Mask ROM 内存区域对象
    char *soc_name;
    int i, base_hartid, hart_count;
    

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
        / 创建 SOC 对象（包含多个 Hart）
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
    quard_star_setup_rom_reset_vec(machine, &s->soc[0], memmap[QUARD_STAR_MROM].base,
                              virt_memmap[QUARD_STAR_MROM].base,
                              virt_memmap[QUARD_STAR_MROM].size,
                              0x0, 0x0);
}


static void quard_star_machine_instance_init(Object *obj)
{
}

static void quard_star_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc); // 将通用对象类指针转换为 MachineClass 指针

    mc->desc = "RISC-V Quard Star board";   // 机器描述信息
    mc->init = quard_star_machine_init;     // 指定机器初始化函数
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
    .instance_size = sizeof(RISCVVirState),
};

static void quard_star_machine_init_register_types(void)
{
    // 将类型注册到 QEMU 的类型系统
    type_register_static(&quard_star_machine_typeinfo);
}
// 在 QEMU 启动时自动调用注册函数
type_init(quard_star_machine_init_register_types)
