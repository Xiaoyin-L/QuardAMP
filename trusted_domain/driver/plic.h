#ifndef PLIC_H
#define PLIC_H

#include <stdint.h>

/*
 * AIA 兼容层：保留 plic_* 名称，内部配置 S-APLIC 与 hart7 S-IMSIC。
 */
void plic_init_hart7(void);

void plic_enable_irq_hart7(int irq);

int plic_claim_hart7(void);

void plic_complete_hart7(int irq);

void plic_set_irq_priority(int irq, uint32_t priority);

#endif /* PLIC_H */
