/* Tiny RGB565 software renderer for a 320x240 screen.  The framebuffer is
 * column-major: pixel (x, y) lives at px[x * GFX_H_PX + y] (see gfx.c). */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#define GFX_W 320
#define GFX_H_PX 240
#define GFX_FONT_W 6 /* 5 px glyph + 1 px spacing */
#define GFX_FONT_H 8

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

typedef struct {
    uint16_t *px; /* GFX_W * GFX_H_PX, column-major */
} gfx_t;

void gfx_clear(gfx_t *g, uint16_t c);
void gfx_pixel(gfx_t *g, int x, int y, uint16_t c);
void gfx_hline(gfx_t *g, int x0, int x1, int y, uint16_t c);
void gfx_vline(gfx_t *g, int x, int y0, int y1, uint16_t c);
void gfx_rect(gfx_t *g, int x, int y, int w, int h, uint16_t c);
void gfx_frame(gfx_t *g, int x, int y, int w, int h, uint16_t c);
void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, uint16_t c);
void gfx_ellipse(gfx_t *g, int cx, int cy, int rx, int ry, uint16_t c);
/* Returns the x just past the drawn text.  max_chars < 0: no limit. */
int gfx_text(gfx_t *g, int x, int y, const char *s, int max_chars, uint16_t c);
int gfx_textn(gfx_t *g, int x, int y, const char *s, int n, uint16_t c);
uint16_t gfx_scale(uint16_t c, unsigned level, unsigned max_level);

#endif
