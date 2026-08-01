#include "sbi.h"

#define SBI_EXT_0_1_SET_TIMER 0x0
#define SBI_EXT_0_1_CONSOLE_PUTCHAR 0x1
#define SBI_EXT_0_1_CLEAR_IPI 0x3
#define SBI_EXT_0_1_SEND_IPI 0x4

static inline long sbi_legacy_call(long ext, unsigned long arg0)
{
    register unsigned long a0 asm("a0") = arg0;
    register unsigned long a7 asm("a7") = ext;

    __asm__ volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (long)a0;
}

void sbi_set_timer(uint64_t stime_value)
{
    sbi_legacy_call(SBI_EXT_0_1_SET_TIMER, (unsigned long)stime_value);
}

void sbi_clear_ipi(void)
{
    sbi_legacy_call(SBI_EXT_0_1_CLEAR_IPI, 0);
}

void sbi_send_ipi(const unsigned long *hart_mask)
{
    sbi_legacy_call(SBI_EXT_0_1_SEND_IPI, (unsigned long)hart_mask);
}

void sbi_console_putchar(int ch)
{
    sbi_legacy_call(SBI_EXT_0_1_CONSOLE_PUTCHAR, (unsigned long)ch);
}
