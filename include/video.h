#ifndef PHOTONOS_VIDEO_H
#define PHOTONOS_VIDEO_H

#include <stdint.h>

#define VIDEO_FONT_WIDTH 8U
#define VIDEO_FONT_HEIGHT 16U

typedef struct {
    uint32_t phys_base_ptr;
    uint16_t x_resolution;
    uint16_t y_resolution;
    uint16_t bytes_per_line;
} __attribute__((packed)) boot_params_t;

extern boot_params_t boot_params;
extern int video_active;

void video_init(void);
void video_put_pixel(int x, int y, uint32_t color);
void video_draw_char(int x, int y, char c, uint32_t fg_color, uint32_t bg_color);
void video_clear(uint32_t color);
void video_scroll(void);
void video_swap_buffers(void);
uint32_t video_width(void);
uint32_t video_height(void);
uint32_t video_stride_pixels(void);
uint32_t video_console_cols(void);
uint32_t video_console_rows(void);

extern int mouse_x;
extern int mouse_y;

#endif
