#include "serial.h"
#include "smp.h"

#include <stdint.h>

#define COM1_PORT 0x3F8

static uint8_t serial_inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int serial_transmit_empty(void)
{
    return (serial_inb(COM1_PORT + 5) & 0x20) != 0;
}

void serial_init(void)
{
    serial_outb(COM1_PORT + 1, 0x00);
    serial_outb(COM1_PORT + 3, 0x80);
    serial_outb(COM1_PORT + 0, 0x03);
    serial_outb(COM1_PORT + 1, 0x00);
    serial_outb(COM1_PORT + 3, 0x03);
    serial_outb(COM1_PORT + 2, 0xC7);
    serial_outb(COM1_PORT + 4, 0x0B);
}

static spinlock_t serial_lock;

void serial_putc(char c)
{
    uint64_t flags = spin_lock_irqsave(&serial_lock);

    if (c == '\n') {
        while (!serial_transmit_empty()) {
        }
        serial_outb(COM1_PORT, (uint8_t)'\r');
    }

    while (!serial_transmit_empty()) {
    }

    serial_outb(COM1_PORT, (uint8_t)c);

    spin_unlock_irqrestore(&serial_lock, flags);
}

void serial_print(char *str)
{
    while (*str) {
        serial_putc(*str++);
    }
}

int serial_received(void)
{
    return (serial_inb(COM1_PORT + 5) & 1) != 0;
}

char serial_getc(void)
{
    if (!serial_received()) {
        return 0;
    }
    return (char)serial_inb(COM1_PORT);
}

extern void vga_puts(const char *str);
extern int video_active;

void klog(char *msg)
{
    serial_print(msg);
    if (video_active) {
        vga_puts(msg);
    }
}
