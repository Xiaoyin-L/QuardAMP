        # qemu -kernel loads the kernel at 0x80000000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80000000.
.section .text
.global _entry
_entry:

        /* OpenSBI 以 S-mode 跳转进来时的寄存器约定（SBI boot protocol）：
         *   a0 = 当前 hartid（由 OpenSBI 从 mhartid 读取后传入）
         *   a1 = fdt 地址（即 DTS 中 next-arg1 = 0x82200000）
         *
         * 原版使用 csrr a1, mhartid 读取 M-mode CSR，
         * 在 S-mode 下会触发 illegal instruction，必须删除。
         * 改为直接使用 a0 作为 hartid。
         */

        mv tp, a0          /* tp 保存 hartid，后续 cpuid() 通过 tp 获取 */


        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + ((hartid + 1) * 4096)

        /* 栈空间分配：每个 hart 拥有独立的 4096 字节栈
         * stack0 是全局栈底，hart N 的栈顶 = stack0 + (N+1)*4096
         * 这样 hart0 用 [stack0, stack0+4096)，hart1 用 [stack0+4096, stack0+8192)...
         */
        /* 根据 hartid 计算栈顶地址 */
        la sp, stack0
        li t0, 1024*4
        addi t1, a0, 1     /* t1 = hartid + 1 */
        mul t0, t0, t1     /* t0 = 4096 * (hartid + 1) */
        add sp, sp, t0     /* sp = stack0 + 4096*(hartid+1)，即该 hart 的栈顶 */
        # jump to start() in start.c
        call start
spin:
        j spin
