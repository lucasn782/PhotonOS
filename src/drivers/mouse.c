#include "mouse.h"
#include "video.h"
#include "serial.h"
#include <stdint.h>

extern void pic_send_eoi(uint8_t irq);

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void mouse_wait_write(void)
{
    // Wait until Input Buffer is empty (Bit 1 of Status is 0)
    while (inb(0x64) & 0x02) {
        __asm__ volatile ("pause");
    }
}

static void mouse_wait_read(void)
{
    // Wait until Output Buffer is full (Bit 0 of Status is 1)
    while (!(inb(0x64) & 0x01)) {
        __asm__ volatile ("pause");
    }
}

static void mouse_write(uint8_t val)
{
    // Tell 8042 to send next byte to the auxiliary mouse device
    mouse_wait_write();
    outb(0x64, 0xD4);
    // Write data byte to data port
    mouse_wait_write();
    outb(0x60, val);
}

static uint8_t mouse_read(void)
{
    mouse_wait_read();
    return inb(0x60);
}

void mouse_init(void)
{
    klog("MOUSE: inicializando controlador 8042...\n");

    // 1. Enable auxiliary mouse port
    mouse_wait_write();
    outb(0x64, 0xA8);

    // 2. Enable Mouse IRQ 12
    // Send "Get Command Byte" command
    mouse_wait_write();
    outb(0x64, 0x20);
    mouse_wait_read();
    uint8_t command_byte = inb(0x60);

    // Set Bit 1 (enable auxiliary device interrupt IRQ 12)
    // Clear Bit 5 (mouse clock disable, which enables clock)
    command_byte |= 0x02;
    command_byte &= ~0x20;

    // Send "Set Command Byte" command
    mouse_wait_write();
    outb(0x64, 0x60);
    mouse_wait_write();
    outb(0x60, command_byte);

    // 3. Set mouse default settings
    mouse_write(0xF6);
    uint8_t ack = mouse_read();
    if (ack != 0xFA) {
        klog("MOUSE: falha ao definir padroes (F6).\n");
    }

    // 4. Enable packet streaming
    mouse_write(0xF4);
    ack = mouse_read();
    if (ack != 0xFA) {
        klog("MOUSE: falha ao habilitar transmissao (F4).\n");
    } else {
        klog("MOUSE: ativado com sucesso.\n");
    }
}

static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];

void mouse_handler(void)
{
    uint8_t status = inb(0x64);

    // Output buffer full (Bit 0) AND Auxiliary device output full (Bit 5)
    if ((status & 0x01) && (status & 0x20)) {
        uint8_t data = inb(0x60);

        if (mouse_cycle == 0 && !(data & 0x08)) {
            // Out of sync: first byte of packet must have bit 3 set to 1
            return;
        }

        mouse_packet[mouse_cycle++] = data;

        if (mouse_cycle == 3) {
            mouse_cycle = 0;

            uint8_t flags = mouse_packet[0];
            int32_t dx = (int32_t)mouse_packet[1];
            int32_t dy = (int32_t)mouse_packet[2];

            // Sign-extend dx and dy
            if (flags & 0x10) {
                dx |= 0xFFFFFF00;
            }
            if (flags & 0x20) {
                dy |= 0xFFFFFF00;
            }

            // Update global coordinates (delta Y is inverted)
            int32_t new_x = mouse_x + dx;
            int32_t new_y = mouse_y - dy;

            uint32_t screen_width = video_width();
            uint32_t screen_height = video_height();
            if (screen_width == 0 || screen_height == 0) {
                return;
            }

            // Strict clamping to the active VBE screen resolution
            if (new_x < 0) new_x = 0;
            if (new_x >= (int32_t)screen_width) new_x = (int32_t)screen_width - 1;
            if (new_y < 0) new_y = 0;
            if (new_y >= (int32_t)screen_height) new_y = (int32_t)screen_height - 1;

            mouse_x = new_x;
            mouse_y = new_y;

            // Trigger double buffer swap
            video_swap_buffers();
        }
    }

    // Acknowledge interrupt to the PICs
    pic_send_eoi(12);
}
