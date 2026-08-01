OUTPUT_ARCH( "riscv" )
ENTRY( _start )

__stack_size = 0x4000;

MEMORY
{
    ram (rwx) : ORIGIN = 0xb0000000, LENGTH = 4M
}

SECTIONS
{
    . = ORIGIN(ram);

    .init :
    {
        KEEP(*(.init))
    } > ram

    .text :
    {
        *(.text.startup .text.startup.*)
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
    } > ram

    .rodata :
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > ram

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > ram

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > ram

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > ram

    . = ALIGN(8);
    PROVIDE(_data_lma = .);
    PROVIDE(_data = .);

    .data :
    {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
        *(.gnu.linkonce.s.*)
        . = ALIGN(8);
    } > ram

    . = ALIGN(8);
    PROVIDE(_edata = .);
    PROVIDE(edata = .);
    PROVIDE(__bss_start = .);

    .bss :
    {
        *(.sbss*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
    } > ram

    PROVIDE(__bss_end = .);
    PROVIDE(_end = .);
    PROVIDE(end = .);
    PROVIDE(heap_start = .);

    .stack ORIGIN(ram) + LENGTH(ram) - __stack_size :
    {
        PROVIDE(heap_end = .);
        . = ALIGN(16);
        . += __stack_size;
        . = ALIGN(16);
        PROVIDE(_sp = .);
    } > ram
}
