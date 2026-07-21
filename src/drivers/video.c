#include <stddef.h>
#include "video.h"
#include "vmm.h"
#include "memory.h"
#include "serial.h"
#include "font_8x16.h"
#include "smp.h"

static spinlock_t video_lock;


#define LFB_VIRTUAL_BASE 0xFFFFFFFFC0000000ULL
#define BACKBUFFER_VIRTUAL_BASE 0xFFFFFFFFC1000000ULL
#define VIDEO_WINDOW_BYTES (BACKBUFFER_VIRTUAL_BASE - LFB_VIRTUAL_BASE)

#define MOUSE_WIDTH 12
#define MOUSE_HEIGHT 19
#define TEXT_CURSOR_WIDTH VIDEO_FONT_WIDTH
#define TEXT_CURSOR_HEIGHT VIDEO_FONT_HEIGHT
#define TEXT_CURSOR_COLOR 0x00FFFFFF

int video_active = 0;
int mouse_x = 512;
int mouse_y = 384;

extern size_t kernel_console_cursor_row(void);
extern size_t kernel_console_cursor_col(void);

static uint32_t framebuffer_width;
static uint32_t framebuffer_height;
static uint32_t framebuffer_pitch;
static uint32_t framebuffer_bytes_per_pixel;
static uint32_t framebuffer_stride_pixels;
static uint64_t framebuffer_lfb_size;
static uint64_t backbuffer_size;

static const uint8_t mouse_cursor_sprite[MOUSE_HEIGHT][MOUSE_WIDTH] = {
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,1,1,1,1,1,1,0},
    {1,2,2,1,2,1,0,0,0,0,0,0},
    {1,2,1,0,1,2,1,0,0,0,0,0},
    {1,1,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,0,1,2,1,0,0,0,0},
    {0,0,0,0,0,1,2,1,0,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0}
};

static void video_memcpy(void *dest, const void *src, size_t size)
{
    uint64_t *d = (uint64_t *)dest;
    const uint64_t *s = (const uint64_t *)src;
    size_t count = size / 8;
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    /* CORRECAO B5: copia os bytes finais nao alinhados a 64-bit */
    size_t tail_offset = count * 8;
    uint8_t *db = (uint8_t *)dest + tail_offset;
    const uint8_t *sb = (const uint8_t *)src + tail_offset;
    size_t tail = size - tail_offset;
    for (size_t i = 0; i < tail; i++) {
        db[i] = sb[i];
    }
}

// Força a invalidação de TLB de uma página específica via hardware
static inline void tlb_flush_page(uintptr_t addr) {
    __asm__ volatile("invlpg (%0)" ::"r"(addr) : "memory");
}

static void video_reset_geometry(void)
{
    framebuffer_width = 0;
    framebuffer_height = 0;
    framebuffer_pitch = 0;
    framebuffer_bytes_per_pixel = 0;
    framebuffer_stride_pixels = 0;
    framebuffer_lfb_size = 0;
    backbuffer_size = 0;
}

static int video_validate_mode(void)
{
    framebuffer_width = boot_params.x_resolution;
    framebuffer_height = boot_params.y_resolution;
    framebuffer_pitch = boot_params.bytes_per_line;

    if (framebuffer_width == 0 || framebuffer_height == 0 ||
        framebuffer_pitch == 0) {
        return 0;
    }

    if (framebuffer_pitch >= framebuffer_width * sizeof(uint32_t) &&
        (framebuffer_pitch & 3U) == 0) {
        framebuffer_bytes_per_pixel = sizeof(uint32_t);
        framebuffer_stride_pixels = framebuffer_pitch / sizeof(uint32_t);
        backbuffer_size = (uint64_t)framebuffer_height *
            (uint64_t)framebuffer_pitch;
    } else if (framebuffer_pitch >= framebuffer_width * 3U) {
        framebuffer_bytes_per_pixel = 3U;
        framebuffer_stride_pixels = framebuffer_width;
        backbuffer_size = (uint64_t)framebuffer_height *
            (uint64_t)framebuffer_stride_pixels * sizeof(uint32_t);
    } else {
        return 0;
    }

    framebuffer_lfb_size = (uint64_t)framebuffer_height *
        (uint64_t)framebuffer_pitch;

    if (framebuffer_lfb_size == 0 ||
        framebuffer_lfb_size > VIDEO_WINDOW_BYTES ||
        backbuffer_size == 0 ||
        backbuffer_size > VIDEO_WINDOW_BYTES) {
        return 0;
    }

    return 1;
}

static uint32_t text_area_width(void)
{
    return video_console_cols() * VIDEO_FONT_WIDTH;
}

static uint32_t text_area_height(void)
{
    return video_console_rows() * VIDEO_FONT_HEIGHT;
}

static void video_fill_rect(uint32_t x0, uint32_t y0, uint32_t width,
    uint32_t height, uint32_t color)
{
    uint32_t *backbuffer = (uint32_t *)BACKBUFFER_VIRTUAL_BASE;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t *row = backbuffer + (y0 + y) * framebuffer_stride_pixels + x0;
        for (uint32_t x = 0; x < width; x++) {
            row[x] = color;
        }
    }
}

void video_init(void)
{
    spin_init(&video_lock);
    video_active = 0;
    video_reset_geometry();

    if (boot_params.phys_base_ptr == 0) {
        return;
    }

    if (!video_validate_mode()) {
        video_reset_geometry();
        klog("VIDEO: modo VBE invalido para LFB linear.\n");
        return;
    }

    uintptr_t phys_addr = boot_params.phys_base_ptr;
    uintptr_t virt_addr = LFB_VIRTUAL_BASE;
    uint32_t lfb_pages = (uint32_t)((framebuffer_lfb_size + 4095ULL) /
        4096ULL);
    uint32_t backbuffer_pages = (uint32_t)((backbuffer_size + 4095ULL) /
        4096ULL);

    for (uint32_t i = 0; i < lfb_pages; i++) {
        uintptr_t target_v = virt_addr + (i * 4096);
        vmm_map(target_v, phys_addr + (i * 4096),
                VMM_PAGE_PRESENT | VMM_PAGE_WRITE | VMM_PAGE_CACHE_DISABLE | VMM_PAGE_WRITE_THROUGH);
        tlb_flush_page(target_v); // CORREÇÃO: Garante sincronia do MMU
    }

    uintptr_t back_virt = BACKBUFFER_VIRTUAL_BASE;
    for (uint32_t i = 0; i < backbuffer_pages; i++) {
        void *phys_page = pmm_alloc();
        if (phys_page == 0) {
            video_reset_geometry();
            klog("VIDEO: erro ao alocar memoria para Backbuffer.\n");
            return;
        }
        uintptr_t target_b = back_virt + (i * 4096);
        vmm_map(target_b, (uintptr_t)phys_page, VMM_PAGE_PRESENT | VMM_PAGE_WRITE);
        tlb_flush_page(target_b); // CORREÇÃO: Sincroniza cache do TLB para o Backbuffer
    }

    video_active = 1;
    mouse_x = (int)(framebuffer_width / 2U);
    mouse_y = (int)(framebuffer_height / 2U);
    video_clear(0x00000000);
    klog("VIDEO: LFB e Backbuffer mapeados e limpos com sucesso.\n");
}

uint32_t video_width(void)
{
    return framebuffer_width;
}

uint32_t video_height(void)
{
    return framebuffer_height;
}

uint32_t video_stride_pixels(void)
{
    return framebuffer_stride_pixels;
}

uint32_t video_console_cols(void)
{
    return framebuffer_width / VIDEO_FONT_WIDTH;
}

uint32_t video_console_rows(void)
{
    return framebuffer_height / VIDEO_FONT_HEIGHT;
}

void video_put_pixel(int x, int y, uint32_t color)
{
    if (!video_active) return;
    if (x < 0 || x >= (int)framebuffer_width || y < 0 || y >= (int)framebuffer_height) return;

    uint64_t flags = spin_lock_irqsave(&video_lock);
    uint32_t *backbuffer = (uint32_t *)BACKBUFFER_VIRTUAL_BASE;
    backbuffer[(uint32_t)y * framebuffer_stride_pixels + (uint32_t)x] = color;
    spin_unlock_irqrestore(&video_lock, flags);
}

void video_draw_char(int x, int y, char c, uint32_t fg_color, uint32_t bg_color)
{
    if (!video_active) return;
    if (x >= (int)framebuffer_width || y >= (int)framebuffer_height) return;
    if (x + (int)VIDEO_FONT_WIDTH <= 0 ||
        y + (int)VIDEO_FONT_HEIGHT <= 0) return;

    uint64_t flags = spin_lock_irqsave(&video_lock);
    uint32_t *backbuffer = (uint32_t *)BACKBUFFER_VIRTUAL_BASE;
    const uint8_t *glyph = font_8x16[(uint8_t)c];

    for (int row = 0; row < (int)VIDEO_FONT_HEIGHT; row++) {
        int py = y + row;
        if (py < 0 || py >= (int)framebuffer_height) {
            continue;
        }

        uint32_t *dest = backbuffer + (uint32_t)py * framebuffer_stride_pixels;
        uint8_t row_data = glyph[row];
        for (int col = 0; col < (int)VIDEO_FONT_WIDTH; col++) {
            int px = x + col;
            if (px < 0 || px >= (int)framebuffer_width) {
                continue;
            }

            int pixel_on = (row_data >> (7 - col)) & 1;
            dest[(uint32_t)px] = pixel_on ? fg_color : bg_color;
        }
    }
    spin_unlock_irqrestore(&video_lock, flags);
}

void video_clear(uint32_t color)
{
    if (!video_active) return;

    uint64_t flags = spin_lock_irqsave(&video_lock);
    video_fill_rect(0, 0, framebuffer_stride_pixels, framebuffer_height, color);
    spin_unlock_irqrestore(&video_lock, flags);
}

void video_scroll(void)
{
    if (!video_active) return;

    uint64_t flags = spin_lock_irqsave(&video_lock);

    uint32_t *backbuffer = (uint32_t *)BACKBUFFER_VIRTUAL_BASE;
    uint32_t width = text_area_width();
    uint32_t height = text_area_height();

    if (width == 0 || height <= VIDEO_FONT_HEIGHT) {
        spin_unlock_irqrestore(&video_lock, flags);
        return;
    }

    uint32_t scroll_lines = height - VIDEO_FONT_HEIGHT;
    for (uint32_t y = 0; y < scroll_lines; y++) {
        uint32_t *dest = backbuffer + y * framebuffer_stride_pixels;
        uint32_t *src = backbuffer + (y + VIDEO_FONT_HEIGHT) *
            framebuffer_stride_pixels;
        for (uint32_t x = 0; x < width; x++) {
            dest[x] = src[x];
        }
    }

    video_fill_rect(0, scroll_lines, width, VIDEO_FONT_HEIGHT, 0x00000000);
    spin_unlock_irqrestore(&video_lock, flags);
}

void video_swap_buffers(void)
{
    if (!video_active) return;

    uint64_t flags = spin_lock_irqsave(&video_lock);


    uint32_t saved_pixels[MOUSE_HEIGHT * MOUSE_WIDTH];
    uint32_t saved_text_cursor[TEXT_CURSOR_HEIGHT * TEXT_CURSOR_WIDTH];
    uint32_t *backbuffer = (uint32_t *)BACKBUFFER_VIRTUAL_BASE;
    uint32_t *lfb = (uint32_t *)LFB_VIRTUAL_BASE;

    int mx = mouse_x;
    int my = mouse_y;
    int text_cursor_drawn = 0;
    uint32_t text_cursor_x = 0;
    uint32_t text_cursor_y = 0;

    for (int y = 0; y < MOUSE_HEIGHT; y++) {
        for (int x = 0; x < MOUSE_WIDTH; x++) {
            int px = mx + x;
            int py = my + y;
            if (px >= 0 && px < (int)framebuffer_width && py >= 0 && py < (int)framebuffer_height) {
                saved_pixels[y * MOUSE_WIDTH + x] =
                    backbuffer[(uint32_t)py * framebuffer_stride_pixels + (uint32_t)px];
            } else {
                saved_pixels[y * MOUSE_WIDTH + x] = 0;
            }
        }
    }

    for (int y = 0; y < MOUSE_HEIGHT; y++) {
        for (int x = 0; x < MOUSE_WIDTH; x++) {
            int px = mx + x;
            int py = my + y;
            if (px >= 0 && px < (int)framebuffer_width && py >= 0 && py < (int)framebuffer_height) {
                uint8_t val = mouse_cursor_sprite[y][x];
                if (val == 1) {
                    backbuffer[(uint32_t)py * framebuffer_stride_pixels + (uint32_t)px] = 0x00000000;
                } else if (val == 2) {
                    backbuffer[(uint32_t)py * framebuffer_stride_pixels + (uint32_t)px] = 0x00FFFFFF;
                }
            }
        }
    }

    size_t cursor_col = kernel_console_cursor_col();
    size_t cursor_row = kernel_console_cursor_row();
    size_t console_cols = video_console_cols();
    size_t console_rows = video_console_rows();
    if (cursor_col < console_cols && cursor_row < console_rows) {
        text_cursor_drawn = 1;
        text_cursor_x = (uint32_t)(cursor_col * VIDEO_FONT_WIDTH);
        text_cursor_y = (uint32_t)(cursor_row * VIDEO_FONT_HEIGHT);

        for (uint32_t y = 0; y < TEXT_CURSOR_HEIGHT; y++) {
            for (uint32_t x = 0; x < TEXT_CURSOR_WIDTH; x++) {
                uint32_t px = text_cursor_x + x;
                uint32_t py = text_cursor_y + y;
                uint32_t offset = y * TEXT_CURSOR_WIDTH + x;

                if (px < framebuffer_width && py < framebuffer_height) {
                    saved_text_cursor[offset] =
                        backbuffer[py * framebuffer_stride_pixels + px];
                    backbuffer[py * framebuffer_stride_pixels + px] =
                        TEXT_CURSOR_COLOR;
                } else {
                    saved_text_cursor[offset] = 0;
                }
            }
        }
    }

    if (framebuffer_bytes_per_pixel == sizeof(uint32_t)) {
        video_memcpy(lfb, backbuffer, (size_t)framebuffer_lfb_size);
    } else {
        uint8_t *lfb_bytes = (uint8_t *)lfb;
        for (uint32_t y = 0; y < framebuffer_height; y++) {
            uint8_t *dest = lfb_bytes + (uint64_t)y * framebuffer_pitch;
            uint32_t *src = backbuffer + (uint64_t)y *
                framebuffer_stride_pixels;
            for (uint32_t x = 0; x < framebuffer_width; x++) {
                uint32_t color = src[x];
                dest[x * 3U] = (uint8_t)(color & 0xFFU);
                dest[x * 3U + 1U] = (uint8_t)((color >> 8) & 0xFFU);
                dest[x * 3U + 2U] = (uint8_t)((color >> 16) & 0xFFU);
            }
        }
    }

    if (text_cursor_drawn) {
        for (uint32_t y = 0; y < TEXT_CURSOR_HEIGHT; y++) {
            for (uint32_t x = 0; x < TEXT_CURSOR_WIDTH; x++) {
                uint32_t px = text_cursor_x + x;
                uint32_t py = text_cursor_y + y;
                if (px < framebuffer_width && py < framebuffer_height) {
                    backbuffer[py * framebuffer_stride_pixels + px] =
                        saved_text_cursor[y * TEXT_CURSOR_WIDTH + x];
                }
            }
        }
    }

    for (int y = 0; y < MOUSE_HEIGHT; y++) {
        for (int x = 0; x < MOUSE_WIDTH; x++) {
            int px = mx + x;
            int py = my + y;
            if (px >= 0 && px < (int)framebuffer_width && py >= 0 && py < (int)framebuffer_height) {
                backbuffer[(uint32_t)py * framebuffer_stride_pixels + (uint32_t)px] =
                    saved_pixels[y * MOUSE_WIDTH + x];
            }
        }
    }

    spin_unlock_irqrestore(&video_lock, flags);
}
