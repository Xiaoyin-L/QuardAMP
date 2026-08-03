#include <stdarg.h>
#include <stdint.h>
#include "debug_log.h"
#include "ns16550.h"

static void debug_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            ns16550_putc('\r');
        }
        ns16550_putc(*s++);
    }
}

static void debug_puthex(unsigned long value)
{
    static const char digits[] = "0123456789abcdef";
    int started = 0;

    debug_puts("0x");
    for (int shift = (int)(sizeof(value) * 8) - 4; shift >= 0; shift -= 4) {
        unsigned int nibble = (unsigned int)((value >> shift) & 0xfUL);
        if (nibble || started || shift == 0) {
            ns16550_putc(digits[nibble]);
            started = 1;
        }
    }
}

static void debug_putdec(long value)
{
    char buf[24];
    int i = 0;
    unsigned long n;

    if (value < 0) {
        ns16550_putc('-');
        n = (unsigned long)(-value);
    } else {
        n = (unsigned long)value;
    }

    do {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    } while (n);

    while (i > 0) {
        ns16550_putc(buf[--i]);
    }
}

void debug_log_init(void)
{
    ns16550_init();
}

void debug_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') {
                ns16550_putc('\r');
            }
            ns16550_putc(*fmt++);
            continue;
        }

        fmt++;
        switch (*fmt++) {
        case 'x':
        case 'p':
            debug_puthex(va_arg(ap, unsigned long));
            break;
        case 'd':
            debug_putdec(va_arg(ap, int));
            break;
        case 's':
            debug_puts(va_arg(ap, const char *));
            break;
        case 'c':
            ns16550_putc((char)va_arg(ap, int));
            break;
        case '%':
            ns16550_putc('%');
            break;
        default:
            ns16550_putc('?');
            break;
        }
    }
    va_end(ap);
}
