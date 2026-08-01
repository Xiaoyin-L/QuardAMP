#ifndef SBI_H
#define SBI_H

#include <stdint.h>

void sbi_set_timer(uint64_t stime_value);
void sbi_clear_ipi(void);
void sbi_send_ipi(const unsigned long *hart_mask);
void sbi_console_putchar(int ch);

#endif
