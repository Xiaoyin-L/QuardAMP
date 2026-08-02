#ifndef NS16550_H
#define NS16550_H

void ns16550_init(void);
void ns16550_putc(char ch);
void ns16550_enable_rx_irq(void);
int ns16550_getc_nonblock(void);

#endif
