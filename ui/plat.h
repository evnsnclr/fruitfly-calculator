/* Platform services the UI needs; implemented for the calculator and host. */
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>

enum {
    PK_UP = 1 << 0,
    PK_DOWN = 1 << 1,
    PK_LEFT = 1 << 2,
    PK_RIGHT = 1 << 3,
    PK_ENTER = 1 << 4,
    PK_ESC = 1 << 5,
    PK_TAB = 1 << 6,
    PK_MENU = 1 << 7,
    PK_PLUS = 1 << 8,
    PK_MINUS = 1 << 9,
    PK_DEL = 1 << 10,
    PK_MUL = 1 << 11,
    PK_DIV = 1 << 12
};

typedef struct {
    uint32_t pressed; /* PK_* keys that went down since the last poll */
    char typed;       /* newly typed character (lower case), 0 if none */
} plat_input_t;

uint32_t plat_ms(void);                 /* monotonic milliseconds */
void plat_present(const uint16_t *fb);  /* show a frame: RGB565, column-major (gfx.h) */
void plat_poll(plat_input_t *in);
void plat_log(const char *line);        /* append a line to the run log */
void plat_sleep(uint32_t ms);           /* frame pacing */
/* Keep a copy of a frame (same layout) for later retrieval; may be a no-op. */
void plat_snapshot(unsigned index, const uint16_t *fb);

#endif
