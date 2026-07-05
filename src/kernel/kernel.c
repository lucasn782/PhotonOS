#include <stdint.h>
#include <stddef.h>

#include "ata.h"
#include "elf.h"
#include "heap.h"
#include "initrd.h"
#include "memory.h"
#include "mutex.h"
#include "net.h"
#include "pci.h"
#include "proc.h"
#include "scheduler.h"
#include "serial.h"
#include "vfs.h"
#include "vmm.h"
#include "video.h"
#include "mouse.h"
#include "apic.h"
#include "smp.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_ATTR 0x0F
#define VGA_CRTC_ADDRESS 0x3D4
#define VGA_CRTC_DATA 0x3D5

#define IDT_ENTRIES 256
#define KERNEL_CODE_SELECTOR 0x08
#define IDT_INTERRUPT_GATE 0x8E
#define IRQ_TIMER_VECTOR 0x20
#define IRQ_KEYBOARD_VECTOR 0x21
#define IA32_EFER 0xC0000080U
#define IA32_STAR 0xC0000081U
#define IA32_LSTAR 0xC0000082U
#define IA32_FMASK 0xC0000084U
#define EFER_SYSCALL_ENABLE 0x1ULL
#define SYSCALL_FMASK 0x200ULL
#define SYSCALL_KERNEL_CS 0x08ULL
#define SYSCALL_USER_STAR_BASE 0x18ULL
#define SYS_WRITE 1ULL
#define SYS_OPEN 2ULL
#define SYS_READ 3ULL
#define SYS_SPAWN 4ULL
#define SYS_EXIT 5ULL
#define SYS_CREATE 6ULL
#define SYS_WAIT 7ULL
#define SYS_PIPE 8ULL
#define SYS_BRK 9ULL
#define SYS_SIGNAL 10ULL
#define SYS_KILL 11ULL
#define SYS_SIGRETURN 12ULL
#define SYS_GETPROCS 13ULL
#define SYS_DUP2 14ULL
#define SYS_CLOSE 15ULL
#define SYS_LIST 16ULL
#define SYS_SOCKET_SEND 17ULL
#define SYS_SOCKET_RECV 18ULL
#define SYS_YIELD 19ULL
#define SYS_GET_TICKS 20ULL
#define SYS_READDIR 21ULL
#define SYS_EXECVE 22ULL
#define SYS_FORK   23ULL
#define SYS_SOCKET 24ULL
#define SYS_BIND   25ULL
#define SYS_CONNECT 26ULL

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI 0x20

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_OUTPUT_FULL 0x01
#define KEYBOARD_QUEUE_SIZE 128
#define PIPE_BUFFER_SIZE 4096
#define USER_HEAP_MAX_SIZE (4ULL * 1024ULL * 1024ULL)

#define VMM_TEST_VIRTUAL_ADDR 0xDEADBEEF000ULL
#define VMM_TEST_VALUE 0x50484F544F4E4F53ULL

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern void keyboard_irq_stub(void);
extern void timer_irq_stub(void);
extern void mouse_irq_stub(void);
extern void tss_install(struct tss64 *tss);
extern void syscall_entry(void);
extern void double_fault_stub(void);
extern void page_fault_stub(void);
extern void tlb_shootdown_stub(void);

static size_t cursor_row;
static size_t cursor_col;
static struct idt_entry idt[IDT_ENTRIES];
static struct tss64 kernel_tss;
static uint8_t double_fault_stack[4096] __attribute__((aligned(4096)));
uint64_t syscall_kernel_rsp0;
static vfs_node_t console_stdin;
static vfs_node_t console_stdout;
static vfs_node_t console_stderr;
static char keyboard_queue[KEYBOARD_QUEUE_SIZE];
static size_t keyboard_queue_read;
static size_t keyboard_queue_write;
static int keyboard_shift;
static int keyboard_ctrl;
static int keyboard_extended;
static int keyboard_altgr;
static uint32_t foreground_pid;
volatile uint64_t kernel_ticks;

size_t kernel_console_cursor_row(void)
{
    return cursor_row;
}

size_t kernel_console_cursor_col(void)
{
    return cursor_col;
}

struct pipe_buffer {
    uint8_t data[PIPE_BUFFER_SIZE];
    size_t read_pos;
    size_t write_pos;
    size_t count;
    mutex_t lock;
};

struct pipe_end {
    struct pipe_buffer *pipe;
    int readable;
    int writable;
};

struct syscall_saved_frame {
    uint64_t r9;
    uint64_t r8;
    uint64_t r10;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r11;
    uint64_t rcx;
    uint64_t user_rsp;
};

// US-QWERTY Keymap - Set 1 PS/2 Scancodes
static const char keymap_normal[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c',
    [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x39] = ' ',
    [0x00] = 0, [0x01] = 0, [0x0E] = 0, [0x0F] = 0,
    [0x1C] = 0, [0x1D] = 0, [0x2A] = 0, [0x36] = 0, [0x3A] = 0,
};

static const char keymap_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '&', [0x08] = '/', [0x09] = '(',
    [0x0A] = ')', [0x0B] = '=', [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C',
    [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x39] = ' ',
    [0x00] = 0, [0x01] = 0, [0x0E] = 0, [0x0F] = 0,
    [0x1C] = 0, [0x1D] = 0, [0x2A] = 0, [0x36] = 0, [0x3A] = 0,
};

static const char keymap_altgr[128] = {
    [0x08] = '{',
    [0x09] = '|',
    [0x0A] = '}',
};

static void memory_set(void *dest, uint8_t value, size_t count)
{
    uint8_t *bytes = dest;
    for (size_t i = 0; i < count; i++) {
        bytes[i] = value;
    }
}

static uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void)
{
    outb(0x80, 0);
}

static void interrupts_disable(void)
{
    __asm__ volatile ("cli");
}

static void interrupts_enable(void)
{
    __asm__ volatile ("sti");
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static uint16_t vga_cell(char ch, uint8_t attr)
{
    return ((uint16_t)attr << 8) | (uint8_t)ch;
}

static void vga_scroll(void)
{
    if (video_active) {
        return;
    }

    if (cursor_row < VGA_HEIGHT) {
        return;
    }

    for (size_t row = 1; row < VGA_HEIGHT; row++) {
        for (size_t col = 0; col < VGA_WIDTH; col++) {
            VGA_MEMORY[(row - 1) * VGA_WIDTH + col] =
                VGA_MEMORY[row * VGA_WIDTH + col];
        }
    }

    for (size_t col = 0; col < VGA_WIDTH; col++) {
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = vga_cell(' ', 0x07);
    }

    cursor_row = VGA_HEIGHT - 1;
}

static void vga_update_hardware_cursor(size_t x, size_t y)
{
    if (video_active) {
        return;
    }

    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);

    outb(VGA_CRTC_ADDRESS, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));

    outb(VGA_CRTC_ADDRESS, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

static size_t graphic_console_cols(void)
{
    uint32_t cols = video_console_cols();
    return cols == 0 ? 1U : (size_t)cols;
}

static size_t graphic_console_rows(void)
{
    uint32_t rows = video_console_rows();
    return rows == 0 ? 1U : (size_t)rows;
}

static void graphic_scroll_if_needed(size_t rows)
{
    if (rows == 0) {
        cursor_row = 0;
        return;
    }

    if (cursor_row < rows) {
        return;
    }

    video_scroll();
    cursor_row = rows - 1U;
}

static void graphic_draw_cell(size_t col, size_t row, char ch)
{
    if (video_console_cols() == 0 || video_console_rows() == 0) {
        return;
    }

    video_draw_char((int)(col * VIDEO_FONT_WIDTH),
        (int)(row * VIDEO_FONT_HEIGHT), ch, 0x00FFFFFF, 0x00000000);
}

static void vga_put_char(char ch)
{
    if (video_active) {
        size_t cols = graphic_console_cols();
        size_t rows = graphic_console_rows();

        if (ch == '\b') {
            if (cursor_col > 0) {
                cursor_col--;
            } else if (cursor_row > 0) {
                cursor_row--;
                cursor_col = cols - 1U;
            } else {
                return;
            }
            graphic_draw_cell(cursor_col, cursor_row, ' ');
            return;
        }

        if (ch == '\n') {
            cursor_col = 0;
            cursor_row++;
            graphic_scroll_if_needed(rows);
            return;
        }

        if (ch == '\r') {
            cursor_col = 0;
            return;
        }

        if (cursor_col >= cols) {
            cursor_col = 0;
            cursor_row++;
        }
        graphic_scroll_if_needed(rows);

        graphic_draw_cell(cursor_col, cursor_row, ch);
        cursor_col++;
        if (cursor_col >= cols) {
            cursor_col = 0;
            cursor_row++;
            graphic_scroll_if_needed(rows);
        }
        return;
    }

    if (ch == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        } else {
            vga_update_hardware_cursor(cursor_col, cursor_row);
            return;
        }

        size_t offset = cursor_row * VGA_WIDTH + cursor_col;
        VGA_MEMORY[offset] = vga_cell(' ', VGA_ATTR);
        vga_update_hardware_cursor(cursor_col, cursor_row);
        return;
    }

    if (ch == '\n') {
        cursor_col = 0;
        cursor_row++;
        vga_scroll();
        vga_update_hardware_cursor(cursor_col, cursor_row);
        return;
    }

    if (ch == '\r') {
        cursor_col = 0;
        vga_update_hardware_cursor(cursor_col, cursor_row);
        return;
    }

    VGA_MEMORY[cursor_row * VGA_WIDTH + cursor_col] = vga_cell(ch, VGA_ATTR);
    cursor_col++;

    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
        vga_scroll();
    }

    vga_update_hardware_cursor(cursor_col, cursor_row);
}

static void vga_clear(void)
{
    if (video_active) {
        video_clear(0x00000000);
        cursor_row = 0;
        cursor_col = 0;
        video_swap_buffers();
        return;
    }

    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEMORY[i] = vga_cell(' ', 0x07);
    }

    cursor_row = 0;
    cursor_col = 0;
    vga_update_hardware_cursor(cursor_col, cursor_row);
}

static void vga_puts(const char *str)
{
    while (*str) {
        vga_put_char(*str++);
    }
    if (video_active) {
        video_swap_buffers();
    }
}

void tss_set_rsp0(uint64_t rsp0)
{
    kernel_tss.rsp0 = rsp0;
    syscall_kernel_rsp0 = rsp0;
}

static void tss_init(void)
{
    memory_set(&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.ist1 = (uint64_t)&double_fault_stack[4096];
    kernel_tss.iomap_base = sizeof(kernel_tss);
    tss_install(&kernel_tss);
}

static void syscall_init(void)
{
    uint64_t efer = rdmsr(IA32_EFER);
    wrmsr(IA32_EFER, efer | EFER_SYSCALL_ENABLE);
    wrmsr(IA32_STAR, (SYSCALL_USER_STAR_BASE << 48) |
        (SYSCALL_KERNEL_CS << 32));
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    wrmsr(IA32_FMASK, SYSCALL_FMASK);
}

vfs_node_t *kernel_stdin_node(void)
{
    return &console_stdin;
}

vfs_node_t *kernel_stdout_node(void)
{
    return &console_stdout;
}

vfs_node_t *kernel_stderr_node(void)
{
    return &console_stderr;
}

static int keyboard_queue_pop(char *ch);
static int copy_user_path(char *dest, const char *src, size_t capacity);

static void memory_copy(void *dest, const void *src, size_t size)
{
    uint8_t *out = dest;
    const uint8_t *in = src;
    for (size_t i = 0; i < size; i++) {
        out[i] = in[i];
    }
}

static int console_read(vfs_node_t *node, uint64_t offset, uint32_t size,
    uint8_t *buffer)
{
    (void)node;
    (void)offset;
    uint32_t read_count = 0;
    while (read_count < size &&
        keyboard_queue_pop((char *)&buffer[read_count])) {
        read_count++;
    }
    return (int)read_count;
}

static size_t console_write(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer)
{
    (void)node;
    (void)offset;
    for (size_t i = 0; i < size; i++) {
        vga_put_char((char)buffer[i]);
        serial_putc((char)buffer[i]);
    }
    if (video_active) {
        video_swap_buffers();
    }
    return size;
}

static int pipe_read(vfs_node_t *node, uint64_t offset, uint32_t size,
    uint8_t *buffer)
{
    (void)offset;
    struct pipe_end *end = node->data;
    if (end == 0 || !end->readable || end->pipe == 0) {
        return 0;
    }

    struct pipe_buffer *pipe = end->pipe;
    uint32_t read_count = 0;

    mutex_lock(&pipe->lock);
    while (read_count < size && pipe->count > 0) {
        buffer[read_count++] = pipe->data[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUFFER_SIZE;
        pipe->count--;
    }

    if (read_count == 0) {
        mutex_unlock(&pipe->lock);
        scheduler_sleep_current(TASK_WAIT_PIPE_READ, (uint64_t)pipe);
    } else {
        scheduler_wake_pipe_writers(pipe);
        mutex_unlock(&pipe->lock);
    }

    return (int)read_count;
}

static size_t pipe_write(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer)
{
    (void)offset;
    struct pipe_end *end = node->data;
    if (end == 0 || !end->writable || end->pipe == 0) {
        return 0;
    }

    struct pipe_buffer *pipe = end->pipe;
    size_t written = 0;

    mutex_lock(&pipe->lock);
    while (written < size && pipe->count < PIPE_BUFFER_SIZE) {
        pipe->data[pipe->write_pos] = buffer[written++];
        pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUFFER_SIZE;
        pipe->count++;
    }

    if (written == 0) {
        mutex_unlock(&pipe->lock);
        scheduler_sleep_current(TASK_WAIT_PIPE_WRITE, (uint64_t)pipe);
    } else {
        scheduler_wake_pipe_readers(pipe);
        mutex_unlock(&pipe->lock);
    }

    return written;
}

static void console_nodes_init(void)
{
    memory_set(&console_stdin, 0, sizeof(console_stdin));
    memory_set(&console_stdout, 0, sizeof(console_stdout));
    memory_set(&console_stderr, 0, sizeof(console_stderr));

    console_stdin.type = VFS_NODE_DEVICE;
    console_stdin.read = console_read;
    console_stdout.type = VFS_NODE_DEVICE;
    console_stdout.write = console_write;
    console_stderr.type = VFS_NODE_DEVICE;
    console_stderr.write = console_write;
}

static void keyboard_queue_push(char ch)
{
    size_t next = (keyboard_queue_write + 1) % KEYBOARD_QUEUE_SIZE;
    if (next == keyboard_queue_read) {
        return;
    }

    keyboard_queue[keyboard_queue_write] = ch;
    keyboard_queue_write = next;
    scheduler_wake_stdin_readers();
}

static int keyboard_queue_pop(char *ch)
{
    if (keyboard_queue_read == keyboard_queue_write) {
        return 0;
    }

    *ch = keyboard_queue[keyboard_queue_read];
    keyboard_queue_read = (keyboard_queue_read + 1) % KEYBOARD_QUEUE_SIZE;
    return 1;
}

static int sys_open(const char *path, int flags)
{
    (void)flags;
    task_t *task = scheduler_current_task();
    char kernel_path[128];

    if (task == 0 || copy_user_path(kernel_path, path, sizeof(kernel_path)) != 0) {
        return -1;
    }

    vfs_node_t *node = vfs_find(kernel_path);
    if (node == 0 || node->type != VFS_NODE_FILE) {
        return -1;
    }

    for (int i = 3; i < TASK_MAX_FDS; i++) {
        if (task->file_descriptors[i] == 0) {
            if (node->open != 0 && node->open(node) != 0) {
                return -1;
            }

            task->file_descriptors[i] = node;
            task->fd_offsets[i] = 0;
            return i;
        }
    }

    return -1;
}

int task_alloc_fd(task_t *task, vfs_node_t *node)
{
    if (task == 0 || node == 0) {
        return -1;
    }

    for (int i = 3; i < TASK_MAX_FDS; i++) {
        if (task->file_descriptors[i] == 0) {
            task->file_descriptors[i] = node;
            task->fd_offsets[i] = 0;
            return i;
        }
    }

    return -1;
}

static int task_owns_stdin(task_t *task)
{
    if (task == 0) {
        return 0;
    }
    return foreground_pid == 0 || foreground_pid == task->pid;
}

static int sys_create(const char *path)
{
    task_t *task = scheduler_current_task();
    char kernel_path[128];

    if (task == 0 || copy_user_path(kernel_path, path, sizeof(kernel_path)) != 0) {
        return -1;
    }

    if (ata_vfs_create(kernel_path) != 0) {
        return -1;
    }

    return sys_open(kernel_path, 0);
}

static int sys_read(int fd, void *buffer, uint32_t size)
{
    task_t *task = scheduler_current_task();

    if (task == 0 || fd < 0 || fd >= TASK_MAX_FDS || buffer == 0 ||
        task->file_descriptors[fd] == 0) {
        return -1;
    }

    vfs_node_t *node = task->file_descriptors[fd];
    if (node == &console_stdin && !task_owns_stdin(task)) {
        return 0;
    }

    uint8_t *kbuf = kmalloc(size);
    if (kbuf == 0 && size > 0) {
        return -1;
    }

    int bytes = vfs_read(node, task->fd_offsets[fd], size, kbuf);

    if (bytes == 0 && node == &console_stdin) {
        scheduler_sleep_current(TASK_WAIT_STDIN, 0);
    }

    if (bytes > 0) {
        for (uint32_t i = 0; i < (uint32_t)bytes; i += 4096) {
            if (!vmm_is_mapped((uint64_t *)task->cr3, (uintptr_t)buffer + i)) {
                if (kbuf != 0) kfree(kbuf);
                return -1;
            }
        }
        if (!vmm_is_mapped((uint64_t *)task->cr3, (uintptr_t)buffer + bytes - 1)) {
            if (kbuf != 0) kfree(kbuf);
            return -1;
        }

        memory_copy(buffer, kbuf, bytes);
        task->fd_offsets[fd] += bytes;
    }

    if (kbuf != 0) {
        kfree(kbuf);
    }

    return bytes;
}

// CORRIGIDO: Fallback de segurança para roteamento garantido do buffer de stdout/stderr gráfico
static int sys_write(int fd, const uint8_t *buffer, size_t size)
{
    task_t *task = scheduler_current_task();

    if (task == 0 || fd < 0 || fd >= TASK_MAX_FDS || buffer == 0) {
        return -1;
    }

    /* Fallback definitivo: roteamento atomico em Ring 0 para stdout (1) e stderr (2) */
    if (fd == 1 || fd == 2) {
        if (!vmm_is_mapped((uint64_t *)task->cr3, (uintptr_t)buffer)) {
            return -1;
        }
        if (size > 1 && !vmm_is_mapped((uint64_t *)task->cr3, (uintptr_t)buffer + size - 1)) {
            return -1;
        }

        for (size_t i = 0; i < size; i++) {
            vga_put_char((char)buffer[i]);
            serial_putc((char)buffer[i]);
        }
        /* CORRECAO B4: video_swap_buffers chamado UMA unica vez apos o loop
         * completo, evitando swap duplo que ocorria quando console_write
         * tambem invocava o swap pelo caminho vfs_write */
        if (video_active) {
            video_swap_buffers();
        }
        return (int)size;
    }

    if (task->file_descriptors[fd] == 0 ||
        task->file_descriptors[fd]->write == 0) {
        return -1;
    }

    if (size > 4096) {
        size = 4096;
    }

    if (!vmm_is_mapped((uint64_t *)task->cr3, (uintptr_t)buffer)) {
        return -1;
    }
    if (size > 1 &&
        !vmm_is_mapped((uint64_t *)task->cr3, (uintptr_t)buffer + size - 1)) {
        return -1;
    }

    uint8_t *kbuf = kmalloc(size);
    if (kbuf == 0 && size > 0) {
        return -1;
    }

    if (size > 0) {
        memory_copy(kbuf, buffer, size);
    }

    size_t bytes = vfs_write(task->file_descriptors[fd],
        task->fd_offsets[fd], size, kbuf);

    if (kbuf != 0) {
        kfree(kbuf);
    }

    if (bytes > 0) {
        task->fd_offsets[fd] += bytes;
    }

    return (int)bytes;
}

static int copy_user_path(char *dest, const char *src, size_t capacity)
{
    if (dest == 0 || src == 0 || capacity == 0) {
        return -1;
    }

    for (size_t i = 0; i < capacity; i++) {
        dest[i] = src[i];
        if (src[i] == '\0') {
            return 0;
        }
    }

    dest[capacity - 1] = '\0';
    return 0;
}

static int sys_spawn(const char *path)
{
    task_t *task = scheduler_current_task();
    char kernel_path[VFS_NAME_MAX];

    if (task == 0 || copy_user_path(kernel_path, path, sizeof(kernel_path)) != 0) {
        return -1;
    }

    vmm_switch_address_space(vmm_kernel_pml4());
    int pid = elf_load_process(kernel_path, 0);
    vmm_switch_address_space((uint64_t *)task->cr3);

    return pid;
}

static int sys_execve(const char *path, const char *const *argv,
    const char *const *envp)
{
    (void)envp;
    task_t *task = scheduler_current_task();
    if (task == 0) {
        return -1;
    }

    char exec_path[128];
    if (copy_user_path(exec_path, path, sizeof(exec_path)) != 0) {
        return -1;
    }

    char command[256];
    size_t cmd_len = 0;

    for (size_t i = 0; exec_path[i] != '\0' && cmd_len < sizeof(command) - 2U; i++) {
        command[cmd_len++] = exec_path[i];
    }

    if (argv != 0) {
        for (size_t argi = 1; cmd_len < sizeof(command) - 2U; argi++) {
            if (!vmm_is_mapped((uint64_t *)task->cr3, (uintptr_t)&argv[argi])) {
                break;
            }

            const char *arg = argv[argi];
            if (arg == 0) {
                break;
            }

            if (!vmm_is_mapped((uint64_t *)task->cr3, (uintptr_t)arg)) {
                break;
            }

            command[cmd_len++] = ' ';

            char arg_buf[64];
            if (copy_user_path(arg_buf, arg, sizeof(arg_buf)) != 0) {
                break;
            }

            for (size_t j = 0; arg_buf[j] != '\0' && cmd_len < sizeof(command) - 1U; j++) {
                command[cmd_len++] = arg_buf[j];
            }
        }
    }

    command[cmd_len] = '\0';

    vmm_switch_address_space(vmm_kernel_pml4());
    int pid = elf_load_process(command, 0);
    vmm_switch_address_space((uint64_t *)task->cr3);

    return pid;
}

static int sys_exit(int status)
{
    scheduler_exit_current(status);
    for (;;) {
        __asm__ volatile ("sti; hlt");
    }
    return 0;
}

static int sys_wait(int pid)
{
    task_t *task = scheduler_current_task();
    int result = scheduler_wait_current((uint32_t)pid);

    if (result > 0) {
        foreground_pid = (uint32_t)pid;
    } else if (task != 0 && foreground_pid == (uint32_t)pid) {
        foreground_pid = task->pid;
    }

    return result;
}

static int is_supported_signal(int signum)
{
    return signum == SIGINT || signum == SIGKILL || signum == SIGTERM;
}

static uintptr_t sys_signal(int signum, uintptr_t handler)
{
    task_t *task = scheduler_current_task();

    if (task == 0 || !is_supported_signal(signum) || signum == SIGKILL) {
        return (uintptr_t)-1;
    }

    uintptr_t previous = task->signal_handlers[signum];
    task->signal_handlers[signum] = handler;
    return previous;
}

static int sys_kill(int pid, int signum)
{
    if (!is_supported_signal(signum)) {
        return -1;
    }

    task_t *target = scheduler_find_task((uint32_t)pid);
    if (target == 0) {
        return -1;
    }

    if (signum == SIGKILL ||
        (target->signal_handlers[signum] == 0 &&
            (signum == SIGINT || signum == SIGTERM))) {
        if (target == scheduler_current_task()) {
            return sys_exit(128 + signum);
        }

        scheduler_terminate_task(target, 128 + signum);
        if (foreground_pid == (uint32_t)pid) {
            task_t *current = scheduler_current_task();
            foreground_pid = current != 0 ? current->pid : 0;
        }
        return 0;
    }

    return scheduler_send_signal((uint32_t)pid, signum);
}

static uint64_t sys_sigreturn(uint64_t syscall_frame)
{
    task_t *task = scheduler_current_task();
    (void)syscall_frame;

    if (task == 0 || task->active_signal == 0) {
        return 0;
    }

    struct task_sigreturn_frame *frame = &task->sigreturn_frame;
    struct task_signal_context *context = &task->signal_context;

    frame->r15 = context->registers.r15;
    frame->r14 = context->registers.r14;
    frame->r13 = context->registers.r13;
    frame->r12 = context->registers.r12;
    frame->r11 = context->registers.r11;
    frame->r10 = context->registers.r10;
    frame->r9 = context->registers.r9;
    frame->r8 = context->registers.r8;
    frame->rbp = context->registers.rbp;
    frame->rdi = context->registers.rdi;
    frame->rsi = context->registers.rsi;
    frame->rdx = context->registers.rdx;
    frame->rcx = context->registers.rcx;
    frame->rbx = context->registers.rbx;
    frame->rax = context->registers.rax;
    frame->rip = context->registers.rip;
    frame->cs = context->cs;
    frame->rflags = context->registers.rflags;
    frame->user_rsp = context->user_rsp;
    frame->ss = context->ss;

    task->active_signal = 0;
    task->signal_context.signum = 0;

    return (uint64_t)frame;
}

static int sys_pipe(int *fds)
{
    task_t *task = scheduler_current_task();
    struct pipe_buffer *pipe;
    struct pipe_end *read_end;
    struct pipe_end *write_end;
    vfs_node_t *read_node;
    vfs_node_t *write_node;
    int read_fd;
    int write_fd;

    if (task == 0 || fds == 0) {
        return -1;
    }

    pipe = kmalloc(sizeof(*pipe));
    read_end = kmalloc(sizeof(*read_end));
    write_end = kmalloc(sizeof(*write_end));
    read_node = kmalloc(sizeof(*read_node));
    write_node = kmalloc(sizeof(*write_node));
    if (pipe == 0 || read_end == 0 || write_end == 0 ||
        read_node == 0 || write_node == 0) {
        return -1;
    }

    memory_set(pipe, 0, sizeof(*pipe));
    memory_set(read_node, 0, sizeof(*read_node));
    memory_set(write_node, 0, sizeof(*write_node));
    mutex_init(&pipe->lock);

    read_end->pipe = pipe;
    read_end->readable = 1;
    read_end->writable = 0;
    write_end->pipe = pipe;
    write_end->readable = 0;
    write_end->writable = 1;

    read_node->type = VFS_NODE_PIPE;
    read_node->data = read_end;
    read_node->read = pipe_read;
    write_node->type = VFS_NODE_PIPE;
    write_node->data = write_end;
    write_node->write = pipe_write;

    read_fd = task_alloc_fd(task, read_node);
    write_fd = task_alloc_fd(task, write_node);
    if (read_fd < 0 || write_fd < 0) {
        return -1;
    }

    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

static uintptr_t align_up_page(uintptr_t value)
{
    return (value + PMM_PAGE_SIZE - 1ULL) & ~(PMM_PAGE_SIZE - 1ULL);
}

static uintptr_t sys_brk(uintptr_t addr)
{
    task_t *task = scheduler_current_task();

    if (task == 0 || task->heap_start == 0) {
        return 0;
    }

    if (addr == 0) {
        return task->heap_end;
    }

    if (addr < task->heap_start ||
        addr > task->heap_start + USER_HEAP_MAX_SIZE) {
        return task->heap_end;
    }

    if (addr <= task->heap_end) {
        task->heap_end = addr;
        return task->heap_end;
    }

    uintptr_t old_page_end = align_up_page(task->heap_end);
    uintptr_t new_page_end = align_up_page(addr);
    uint64_t restore_cr3 = task->cr3;

    vmm_switch_address_space(vmm_kernel_pml4());

    for (uintptr_t page = old_page_end; page < new_page_end;
        page += PMM_PAGE_SIZE) {
        void *physical = pmm_alloc();
        if (physical == 0 || task->user_page_count >= TASK_MAX_USER_PAGES) {
            if (physical != 0) {
                pmm_free(physical);
            }
            vmm_switch_address_space((uint64_t *)restore_cr3);
            return task->heap_end;
        }

        memory_set(physical, 0, PMM_PAGE_SIZE);
        task->user_physical_pages[task->user_page_count++] =
            (uint64_t)physical;
        vmm_map_in_space((uint64_t *)task->cr3, page, (uintptr_t)physical,
            PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        vmm_flush(page);
    }

    vmm_switch_address_space((uint64_t *)restore_cr3);
    task->heap_end = addr;
    return task->heap_end;
}

static int sys_dup2(int old_fd, int new_fd)
{
    task_t *task = scheduler_current_task();

    if (task == 0 || old_fd < 0 || old_fd >= TASK_MAX_FDS ||
        new_fd < 0 || new_fd >= TASK_MAX_FDS ||
        task->file_descriptors[old_fd] == 0) {
        return -1;
    }

    task->file_descriptors[new_fd] = task->file_descriptors[old_fd];
    task->fd_offsets[new_fd] = task->fd_offsets[old_fd];
    return new_fd;
}

static int sys_close(int fd)
{
    task_t *task = scheduler_current_task();

    if (task == 0 || fd < 0 || fd >= TASK_MAX_FDS ||
        task->file_descriptors[fd] == 0) {
        return -1;
    }

    vfs_node_t *node = task->file_descriptors[fd];
    task->file_descriptors[fd] = 0;
    task->fd_offsets[fd] = 0;

    if (node->close != 0) {
        node->close(node);
    }
    return 0;
}

static uint32_t proc_state_from_task(enum task_state state)
{
    if (state == TASK_READY) {
        return PROC_STATE_READY;
    }
    if (state == TASK_RUNNING) {
        return PROC_STATE_RUNNING;
    }
    if (state == TASK_SLEEPING) {
        return PROC_STATE_SLEEPING;
    }
    if (state == TASK_WAITING) {
        return PROC_STATE_WAITING;
    }
    if (state == TASK_BLOCKED) {
        return PROC_STATE_BLOCKED;
    }
    return PROC_STATE_ZOMBIE;
}

static void proc_copy_name(char *dest, const char *src)
{
    size_t i = 0;
    while (src[i] != '\0' && i < PROC_NAME_MAX - 1U) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static int sys_getprocs(void *user_buffer, size_t max_size)
{
    if (user_buffer == 0 || max_size < sizeof(proc_info_t)) {
        return -1;
    }

    proc_info_t *out = user_buffer;
    size_t capacity = max_size / sizeof(proc_info_t);
    size_t written = 0;

    scheduler_task_table_lock();
    uint32_t count = scheduler_task_count();
    for (uint32_t i = 0; i < count && written < capacity; i++) {
        task_t *task = scheduler_task_at(i);
        if (task == 0 || task->state == TASK_ZOMBIE) {
            continue;
        }

        out[written].pid = task->pid;
        out[written].state = proc_state_from_task(task->state);
        out[written].flags = task->pid == foreground_pid ?
            PROC_FLAG_FOREGROUND : 0;
        proc_copy_name(out[written].name, task->name);
        written++;
    }
    scheduler_task_table_unlock();

    return (int)written;
}

static int sys_list(const char *path, uint8_t *buffer, size_t size)
{
    vfs_node_t *dir = vfs_find(path);
    size_t written = 0;

    if (dir == 0 || dir->type != VFS_NODE_DIRECTORY || buffer == 0) {
        return -1;
    }

    for (vfs_node_t *node = dir->child; node != 0; node = node->sibling) {
        for (size_t i = 0; node->name[i] != '\0'; i++) {
            if (written >= size) {
                return (int)written;
            }
            buffer[written++] = (uint8_t)node->name[i];
        }

        if (written >= size) {
            return (int)written;
        }
        buffer[written++] = '\n';
    }

    return (int)written;
}

static int sys_readdir(int fd, vfs_dir_entry_t *buf, uint32_t count)
{
    task_t *task = scheduler_current_task();

    if (task == 0 || fd < 0 || fd >= TASK_MAX_FDS || buf == 0 ||
        task->file_descriptors[fd] == 0) {
        return -1;
    }

    vfs_node_t *node = task->file_descriptors[fd];
    if (node->type != VFS_NODE_DIRECTORY) {
        return -1;
    }

    uint32_t read_count = 0;
    uint32_t index = (uint32_t)task->fd_offsets[fd];

    while (read_count < count) {
        vfs_dir_entry_t entry;
        int ret = vfs_readdir(node, index, &entry);
        if (ret < 0) {
            return -1;
        }
        if (ret == 0) {
            break;
        }
        buf[read_count] = entry;
        read_count++;
        index++;
    }

    task->fd_offsets[fd] = index;
    return (int)read_count;
}

static int sys_fork(uint64_t frame_addr)
{
    return scheduler_fork_current(frame_addr);
}

uint64_t syscall_handler(uint64_t number, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    (void)arg4;
    (void)arg5;
    uint64_t ret = (uint64_t)-1;

    if (number == SYS_OPEN) {
        ret = (uint64_t)sys_open((const char *)arg1, (int)arg2);
    }
    else if (number == SYS_READ) {
        ret = (uint64_t)sys_read((int)arg1, (void *)arg2, (uint32_t)arg3);
    }
    else if (number == SYS_SPAWN) {
        ret = (uint64_t)sys_spawn((const char *)arg1);
    }
    else if (number == SYS_EXIT) {
        ret = (uint64_t)sys_exit((int)arg1);
    }
    else if (number == SYS_WRITE) {
        ret = (uint64_t)sys_write((int)arg1, (const uint8_t *)arg2,
            (size_t)arg3);
    }
    else if (number == SYS_CREATE) {
        ret = (uint64_t)sys_create((const char *)arg1);
    }
    else if (number == SYS_WAIT) {
        ret = (uint64_t)sys_wait((int)arg1);
    }
    else if (number == SYS_PIPE) {
        ret = (uint64_t)sys_pipe((int *)arg1);
    }
    else if (number == SYS_BRK) {
        ret = (uint64_t)sys_brk((uintptr_t)arg1);
    }
    else if (number == SYS_SIGNAL) {
        ret = (uint64_t)sys_signal((int)arg1, (uintptr_t)arg2);
    }
    else if (number == SYS_KILL) {
        ret = (uint64_t)sys_kill((int)arg1, (int)arg2);
    }
    else if (number == SYS_SIGRETURN) {
        return sys_sigreturn(arg6);
    }
    else if (number == SYS_GETPROCS) {
        ret = (uint64_t)sys_getprocs((void *)arg1, (size_t)arg2);
    }
    else if (number == SYS_DUP2) {
        ret = (uint64_t)sys_dup2((int)arg1, (int)arg2);
    }
    else if (number == SYS_CLOSE) {
        ret = (uint64_t)sys_close((int)arg1);
    }
    else if (number == SYS_LIST) {
        ret = (uint64_t)sys_list((const char *)arg1, (uint8_t *)arg2,
            (size_t)arg3);
    }
    else if (number == SYS_SOCKET_SEND) {
        ret = (uint64_t)sys_socket_send((uint32_t)arg1, (uint8_t)arg2,
            (const void *)arg3, (size_t)arg4);
    }
    else if (number == SYS_SOCKET_RECV) {
        ret = (uint64_t)sys_socket_recv((uint8_t)arg1, (void *)arg2,
            (size_t)arg3);
    }
    else if (number == SYS_YIELD) {
        scheduler_yield();
        ret = 0;
    }
    else if (number == SYS_GET_TICKS) {
        ret = kernel_ticks;
    }
    else if (number == SYS_READDIR) {
        ret = (uint64_t)sys_readdir((int)arg1, (vfs_dir_entry_t *)arg2, (uint32_t)arg3);
    }
    else if (number == SYS_EXECVE) {
        ret = (uint64_t)sys_execve((const char *)arg1, (const char *const *)arg2,
            (const char *const *)arg3);
    }
    else if (number == SYS_FORK) {
        ret = (uint64_t)sys_fork(arg6);
    }
    else if (number == SYS_SOCKET) {
        ret = (uint64_t)sys_socket((int)arg1, (int)arg2, (int)arg3);
    }
    else if (number == SYS_BIND) {
        ret = (uint64_t)sys_bind((int)arg1, (const struct sockaddr *)arg2, (uint32_t)arg3);
    }
    else if (number == SYS_CONNECT) {
        ret = (uint64_t)sys_connect((int)arg1, (const struct sockaddr *)arg2, (uint32_t)arg3);
    }

    scheduler_handle_syscall_signals(arg6, ret);
    return ret;
}

static void vga_put_u64(uint64_t value)
{
    char digits[20];
    size_t length = 0;

    if (value == 0) {
        vga_put_char('0');
        if (video_active) {
            video_swap_buffers();
        }
        return;
    }

    while (value > 0 && length < sizeof(digits)) {
        digits[length++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }

    while (length > 0) {
        vga_put_char(digits[--length]);
    }
    if (video_active) {
        video_swap_buffers();
    }
}

static void vmm_self_test(void)
{
    void *physical = pmm_alloc();
    if (physical == NULL) {
        klog("VMM: falha ao alocar frame fisico para teste.\n");
        vga_puts("VMM: Falha ao alocar frame fisico para teste. [ERRO]\n");
        return;
    }

    vmm_map(VMM_TEST_VIRTUAL_ADDR, (uintptr_t)physical,
        PAGE_PRESENT | PAGE_WRITABLE);
    klog("VMM: Pagina Mapeada.\n");

    volatile uint64_t *mapped = (volatile uint64_t *)VMM_TEST_VIRTUAL_ADDR;
    *mapped = VMM_TEST_VALUE;

    if (*mapped == VMM_TEST_VALUE) {
        klog("VMM: leitura bate com o valor escrito. [OK]\n");
        vga_puts("VMM: Mapeamento dinamico validado. [OK]\n");
    } else {
        klog("VMM: leitura diferente do valor escrito. [ERRO]\n");
        vga_puts("VMM: Escrita no mapeamento falhou. [ERRO]\n");
    }
}

static void idt_set_gate(uint8_t vector, void (*handler)(void))
{
    uint64_t address = (uint64_t)handler;

    idt[vector].offset_low = (uint16_t)(address & 0xFFFFULL);
    idt[vector].selector = KERNEL_CODE_SELECTOR;
    idt[vector].ist = 0;
    idt[vector].type_attr = IDT_INTERRUPT_GATE;
    idt[vector].offset_mid = (uint16_t)((address >> 16) & 0xFFFFULL);
    idt[vector].offset_high = (uint32_t)((address >> 32) & 0xFFFFFFFFULL);
    idt[vector].zero = 0;
}

void idt_load(void)
{
    struct idt_pointer idtr;
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

static void vga_put_hex(uint64_t value)
{
    char hex_digits[] = "0123456789ABCDEF";
    char buffer[16];

    for (int i = 15; i >= 0; i--) {
        buffer[i] = hex_digits[value & 0xF];
        value >>= 4;
    }

    /* CORRECAO B3: usa vga_put_char direto para '0' e 'x' evitando
     * que vga_puts("0x") dispare video_swap_buffers antes dos digitos */
    vga_put_char('0');
    vga_put_char('x');
    for (int i = 0; i < 16; i++) {
        vga_put_char(buffer[i]);
    }
    if (video_active) {
        video_swap_buffers();
    }
}

static void klog_hex(uint64_t value)
{
    char hex_digits[] = "0123456789ABCDEF";
    char buffer[16];

    for (int i = 15; i >= 0; i--) {
        buffer[i] = hex_digits[value & 0xF];
        value >>= 4;
    }

    klog("0x");
    for (int i = 0; i < 16; i++) {
        serial_putc(buffer[i]);
    }
}

void double_fault_handler(uint64_t rip, uint64_t cs, uint64_t rflags, uint64_t rsp, uint64_t ss, uint64_t error_code)
{
    __asm__ volatile ("cli");

    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

    vga_clear();
    vga_puts("================ KERNEL PANIC: DOUBLE FAULT (INT 0x08) ================\n");
    vga_puts("A pilha de kernel estourou ou ocorreu uma falha grave em Ring 0.\n");
    vga_puts("Estado da CPU no momento da falha:\n");
    vga_puts("   RIP: "); vga_put_hex(rip); vga_puts("\n");
    vga_puts("   CS:  "); vga_put_hex(cs);  vga_puts("\n");
    vga_puts("   RSP: "); vga_put_hex(rsp); vga_puts("\n");
    vga_puts("   SS:  "); vga_put_hex(ss);  vga_puts("\n");
    vga_puts("   RFLAGS: "); vga_put_hex(rflags); vga_puts("\n");
    vga_puts("   Error Code: "); vga_put_hex(error_code); vga_puts("\n");
    vga_puts("\nRegistradores de controle:\n");
    vga_puts("   CR0: "); vga_put_hex(cr0); vga_puts("\n");
    vga_puts("   CR2: "); vga_put_hex(cr2); vga_puts("\n");
    vga_puts("   CR3: "); vga_put_hex(cr3); vga_puts("\n");
    vga_puts("   CR4: "); vga_put_hex(cr4); vga_puts("\n");
    vga_puts("========================================================================\n");

    klog("\n*** KERNEL PANIC: DOUBLE FAULT (INT 0x08) ***\n");
    klog("Falha grave ou estouro de pilha de kernel detectado.\n");
    klog("   RIP: "); klog_hex(rip); klog("\n");
    klog("   CS:  "); klog_hex(cs);  klog("\n");
    klog("   RSP: "); klog_hex(rsp); klog("\n");
    klog("   SS:  "); klog_hex(ss);  klog("\n");
    klog("   RFLAGS: "); klog_hex(rflags); klog("\n");
    klog("   Error Code: "); klog_hex(error_code); klog("\n");
    klog("   CR0: "); klog_hex(cr0); klog("\n");
    klog("   CR2: "); klog_hex(cr2); klog("\n");
    klog("   CR3: "); klog_hex(cr3); klog("\n");
    klog("   CR4: "); klog_hex(cr4); klog("\n");
    klog("O sistema foi interrompido.\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void idt_init(void)
{
    memory_set(idt, 0, sizeof(idt));
    idt_set_gate(IRQ_TIMER_VECTOR, timer_irq_stub);
    idt_set_gate(IRQ_KEYBOARD_VECTOR, keyboard_irq_stub);
    idt_set_gate(0x2C, mouse_irq_stub); 
    idt_set_gate(8, double_fault_stub);
    idt[8].ist = 1;
    idt_set_gate(14, page_fault_stub);
    idt_set_gate(0x79, tlb_shootdown_stub);
    idt_load();
}

static void pic_init_irqs(void)
{
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    outb(PIC1_DATA, 0xF8); 
    outb(PIC2_DATA, 0xEF); 
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
    if (apic_is_enabled()) {
        apic_eoi();
    }
}

static char keyboard_char_from_scancode(uint8_t scancode)
{
    if (scancode >= 128) {
        return 0;
    }

    char ch = 0;
    if (keyboard_altgr) {
        ch = keymap_altgr[scancode];
        if (ch != 0) {
            return ch;
        }
    }

    const char *keymap = keyboard_shift ? keymap_shift : keymap_normal;
    return keymap[scancode];
}

static task_t *keyboard_foreground_task(void)
{
    if (foreground_pid != 0) {
        task_t *task = scheduler_find_task(foreground_pid);
        if (task != 0) {
            return task;
        }
        foreground_pid = 0;
    }
    return scheduler_current_task();
}

static void keyboard_send_sigint(void)
{
    task_t *task = keyboard_foreground_task();
    if (task == 0) {
        klog("keyboard_send_sigint: no foreground task\n");
        return;
    }

    klog("keyboard_send_sigint: enviando SIGINT\n");
    scheduler_send_signal(task->pid, SIGINT);
}

static void keyboard_handle_scancode(uint8_t scancode)
{
    if (scancode == 0xE0) {
        keyboard_extended = 1;
        return;
    }

    if (keyboard_extended) {
        keyboard_extended = 0;
        if (scancode == 0x38) {
            keyboard_altgr = 1;
            return;
        }
        if (scancode == 0xB8) {
            keyboard_altgr = 0;
            return;
        }
        return;
    }

    if (scancode & 0x80) {
        uint8_t make_code = scancode & 0x7F;
        if (make_code == 0x2A || make_code == 0x36) {
            keyboard_shift = 0;
        }
        if (make_code == 0x1D) {
            keyboard_ctrl = 0;
        }
        return;
    }

    if (scancode == 0x2A || scancode == 0x36) {
        keyboard_shift = 1;
        return;
    }

    if (scancode == 0x1D) {
        keyboard_ctrl = 1;
        return;
    }

    if (keyboard_ctrl && scancode == 0x2E) {
        keyboard_send_sigint();
        return;
    }

    if (scancode == 0x0E) {
        keyboard_queue_push('\b');
        return;
    }

    if (scancode == 0x1C) {
        keyboard_queue_push('\n');
        return;
    }

    if (scancode == 0x0F) {
        keyboard_queue_push('\t');
        return;
    }

    char ch = keyboard_char_from_scancode(scancode);
    if (ch != 0) {
        keyboard_queue_push(ch);
    }
}

static void keyboard_flush(void)
{
    for (uint8_t i = 0; i < 16; i++) {
        if ((inb(KEYBOARD_STATUS_PORT) & KEYBOARD_OUTPUT_FULL) == 0) {
            break;
        }
        (void)inb(KEYBOARD_DATA_PORT);
    }
}

static void keyboard_init(void)
{
    keyboard_shift = 0;
    keyboard_ctrl = 0;
    keyboard_extended = 0;
    keyboard_altgr = 0;
    keyboard_queue_read = 0;
    keyboard_queue_write = 0;
    idt_init();
    pic_init_irqs();
    keyboard_flush();
}

void keyboard_irq_handler(void)
{
    if (inb(KEYBOARD_STATUS_PORT) & KEYBOARD_OUTPUT_FULL) {
        keyboard_handle_scancode(inb(KEYBOARD_DATA_PORT));
    }
    pic_send_eoi(1);
}

void kmain(void)
{
    interrupts_disable();
    serial_init();
    klog("PhotonOS: serial COM1 ativo.\n");
    vga_clear();
    vga_puts("PhotonOS: Kernel em C operando em Long Mode\n");
    
    pmm_init();
    klog("PMM: inicializado.\n");
    vga_puts("PMM: Gerenciador de memoria fisica ativo. [OK]\n");
    vga_puts("PMM: Memoria livre detectada: ");
    vga_put_u64(pmm_free_memory_mib());
    vga_puts(" MiB\n");
    
    vmm_init();
    klog("VMM Iniciado.\n");
    
    // CORRIGIDO: Força a sincronia limpando os cursores após inicializar o buffer gráfico VBE
    video_init();
    vga_clear(); 
    
    vmm_self_test();
    heap_init();
    klog("Heap: kmalloc/kfree inicializados.\n");
    
    vfs_init();
    initrd_init();
    if (ata_init()) {
        ata_vfs_init();
    }
    
    int network_ready = pci_init() == 0;
    if (network_ready) {
        net_init();
    }
    console_nodes_init();
    klog("VFS: initrd e armazenamento persistente inicializados.\n");

    apic_init();
    klog("APIC: PIC legado desativado. Local APIC mapeado e ativo.\n");

    smp_init();
    klog("SMP: trampolim instalado. Inicializando APs...\n");
    smp_boot_ap(1);
    smp_boot_ap(2);
    smp_boot_ap(3);
    
    tss_init();
    syscall_init();
    klog("Syscall/TSS: estruturas de Ring 3 inicializadas.\n");
    
    scheduler_init();
    if (network_ready) {
        if (scheduler_create_task(net_kernel_thread) >= 0) {
            klog("NET: Thread de rede registrada no escalonador.\n");
        } else {
            klog("NET: falha ao registrar thread de rede.\n");
        }
    }
    
    int shell_pid = elf_load_process("/bin/shell", 0);
    if (shell_pid >= 0) {
        foreground_pid = (uint32_t)shell_pid;
        task_t *shell_task = scheduler_find_task(shell_pid);
        if (shell_task != 0) {
            shell_task->state = TASK_READY;
        }
        klog("ELF: /bin/shell carregado em Ring 3.\n");
    } else {
        klog("ELF: falha ao carregar /bin/shell.\n");
    }
    
    klog("Scheduler: Round-Robin com shell ELF inicializado.\n");
    keyboard_init();
    mouse_init();
    interrupts_enable();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
