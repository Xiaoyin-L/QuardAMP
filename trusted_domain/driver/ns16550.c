#include <stdint.h>
#include "ns16550.h"
#include "quard_star.h"

#define UART_RHR 0
#define UART_THR 0
#define UART_LSR 5
#define UART_LSR_THRE 0x20

static volatile uint8_t * const uart = (volatile uint8_t *)UART2_ADDR;

void ns16550_init(void)
{
}

void ns16550_putc(char ch)
{
    while ((uart[UART_LSR] & UART_LSR_THRE) == 0) {
    }

    uart[UART_THR] = (uint8_t)ch;
}
