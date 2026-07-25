    .section .text
    .global _start
    .type _start,@function # 声明函数类型符号

_start:
    # mhartid 是 Machine Mode CSR，表示当前硬件线程编号
    csrr a0, mhartid # 获取当前 hart 的 ID
    li		t0,	0x0     
	beq		a0, t0, _core0
_loop:
    j       _loop
_core0:
    li   t0, 0x100
    slli t0, t0, 20 # t0 = 0x100 << 20, 即uart0的基地址
# 向 UART 寄存器写字符
    li	    t1, 'H'
# sb 是 RISC-V 的 Store Byte 指令，只写一个字节
    sb      t1, 0(t0) # 将 t1 的最低 8 bit 写入地址 0x10000000
    li		t1,	'e'
	sb		t1, 0(t0)
	li		t1,	'l'
	sb		t1, 0(t0)
	li		t1,	'l'
	sb		t1, 0(t0)
	li		t1,	'o'
	sb		t1, 0(t0)
	li		t1,	' '
	sb		t1, 0(t0)
	li		t1,	'Q'
	sb		t1, 0(t0)
	li		t1,	'u'
	sb		t1, 0(t0)
	li		t1,	'a'
	sb		t1, 0(t0)
	li		t1,	'r'
	sb		t1, 0(t0)
	li		t1,	'd'
	sb		t1, 0(t0)
	li		t1,	' '
	sb		t1, 0(t0)
	li		t1,	'S'
	sb		t1, 0(t0)
	li		t1,	't'
	sb		t1, 0(t0)
	li		t1,	'a'
	sb		t1, 0(t0)
	li		t1,	'r'
	sb		t1, 0(t0)
	li		t1,	' '
	sb		t1, 0(t0)
	li		t1,	'b'
	sb		t1, 0(t0)
	li		t1,	'o'
	sb		t1, 0(t0)
	li		t1,	'a'
	sb		t1, 0(t0)
	li		t1,	'r'
	sb		t1, 0(t0)
	li		t1,	'd'
	sb		t1, 0(t0)
	li		t1,	'!'
	sb		t1, 0(t0)
	li		t1,	'\n'
	sb		t1, 0(t0)
    j       _loop

    .end # 表示当前汇编源文件结束