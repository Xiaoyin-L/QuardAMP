#include <stdint.h>
#include "ns16550.h"
#include "quard_star.h"

#define UART_RHR 0
#define UART_THR 0
#define UART_IER 1
#define UART_FCR 2
#define UART_LCR 3
#define UART_LSR 5
#define UART_IER_RDA 0x01
#define UART_FCR_FIFO_ENABLE 0x01
#define UART_FCR_CLEAR_RCVR 0x02
#define UART_FCR_CLEAR_XMIT 0x04
#define UART_LCR_8N1 0x03
#define UART_LSR_DR 0x01
#define UART_LSR_THRE 0x20

static volatile uint8_t * const uart = (volatile uint8_t *)UART2_ADDR;

void ns16550_init(void)
{
    /*
     * 启动阶段先关闭设备侧中断，避免 PLIC/handler 尚未准备好时
     * UART 已经产生 RX interrupt。
     */
    uart[UART_IER] = 0;
    /*
     * 8 data bits, no parity, 1 stop bit。
     * 这里只设置基本帧格式，不改 baud divisor，避免破坏 QEMU 已经可用的串口输出。
     */
    uart[UART_LCR] = UART_LCR_8N1;
    /*
     * 打开 FIFO，并清掉启动前残留的 RX/TX FIFO 内容。
     */
    uart[UART_FCR] = UART_FCR_FIFO_ENABLE | UART_FCR_CLEAR_RCVR |
                     UART_FCR_CLEAR_XMIT;
}

void ns16550_putc(char ch)
{
    while ((uart[UART_LSR] & UART_LSR_THRE) == 0) {
    }

    uart[UART_THR] = (uint8_t)ch;
}

void ns16550_enable_rx_irq(void)
{
    uart[UART_IER] = uart[UART_IER] | UART_IER_RDA;
}

int ns16550_getc_nonblock(void)
{
    if ((uart[UART_LSR] & UART_LSR_DR) == 0) {
        return -1;
    }

    return (int)uart[UART_RHR];
}
