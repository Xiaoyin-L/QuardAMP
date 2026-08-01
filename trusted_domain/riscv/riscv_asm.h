#ifndef RISCV_ASM_H
#define RISCV_ASM_H

#include "riscv_encoding.h"

#ifdef __ASSEMBLER__
#if __riscv_xlen == 64
#define LOAD ld
#define STOR sd
#define REGSIZE 8
#else
#define LOAD lw
#define STOR sw
#define REGSIZE 4
#endif
#else
#define __ASM_STR(x) #x
#define _ASM_STR(x) __ASM_STR(x)

#define csr_read(csr)                                                   \
    ({                                                                  \
        unsigned long __v;                                              \
        __asm__ volatile ("csrr %0, " _ASM_STR(csr) : "=r"(__v) : : "memory"); \
        __v;                                                            \
    })

#define csr_write(csr, val)                                             \
    ({                                                                  \
        unsigned long __v = (unsigned long)(val);                       \
        __asm__ volatile ("csrw " _ASM_STR(csr) ", %0" : : "rK"(__v) : "memory"); \
    })

#define csr_set(csr, val)                                               \
    ({                                                                  \
        unsigned long __v = (unsigned long)(val);                       \
        __asm__ volatile ("csrs " _ASM_STR(csr) ", %0" : : "rK"(__v) : "memory"); \
    })

#define csr_clear(csr, val)                                             \
    ({                                                                  \
        unsigned long __v = (unsigned long)(val);                       \
        __asm__ volatile ("csrc " _ASM_STR(csr) ", %0" : : "rK"(__v) : "memory"); \
    })
#endif

#endif
