/*
 * ui_preview — drive the calculator UI headlessly on the host.
 *
 *   ui_preview <data-dir> <out-dir> [--arena-mb N] <command>...
 *     f:N      run N frames          k:NAME   press a key (up, down, left,
 *     t:C      type character C               right, enter, esc, tab, menu,
 *     s:NAME   save <out-dir>/NAME.ppm        plus, minus, del, mul, div)
 *
 * Time is virtual so runs are reproducible: each clock read costs CLOCK_MS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

#define CLOCK_MS 4u

static uint32_t now_ms;
static plat_input_t pending;
static uint16_t fb[GFX_W * GFX_H_PX];
static uint16_t shown[GFX_W * GFX_H_PX];
static fs_sim_t sim;
static app_t app;

uint32_t plat_ms(void) { return now_ms += CLOCK_MS; }
void plat_sleep(uint32_t ms) { now_ms += ms; }
void plat_present(const uint16_t *f) { memcpy(shown, f, sizeof shown); }
void plat_poll(plat_input_t *in) {
    *in = pending;
    memset(&pending, 0, sizeof pending);
}
void plat_log(const char *line) { fprintf(stderr, "log: %s\n", line); }
void plat_snapshot(unsigned index, const uint16_t *f) {
    (void)index;
    (void)f;
}

static int save_ppm(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", GFX_W, GFX_H_PX);
    for (int y = 0; y < GFX_H_PX; y++) {
        for (int x = 0; x < GFX_W; x++) {
            unsigned px = shown[x * GFX_H_PX + y]; /* frames are column-major */
            unsigned char rgb[3] = {(unsigned char)(((px >> 11) & 31u) * 255u / 31u),
                                    (unsigned char)(((px >> 5) & 63u) * 255u / 63u), (unsigned char)((px & 31u) * 255u / 31u)};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}

static uint32_t key_bit(const char *name) {
    static const struct { const char *name; uint32_t bit; } KEYS[] = {
        {"up", PK_UP}, {"down", PK_DOWN}, {"left", PK_LEFT}, {"right", PK_RIGHT}, {"enter", PK_ENTER},
        {"esc", PK_ESC}, {"tab", PK_TAB}, {"menu", PK_MENU}, {"plus", PK_PLUS}, {"minus", PK_MINUS},
        {"del", PK_DEL}, {"mul", PK_MUL}, {"div", PK_DIV},
    };
    for (unsigned i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (!strcmp(name, KEYS[i].name)) return KEYS[i].bit;
    return 0;
}

static int frame(void) {
    plat_input_t in;
    plat_poll(&in);
    return app_frame(&app, &in);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ui_preview <data-dir> <out-dir> [--arena-mb N] <command>...\n");
        return 2;
    }
    int first = 3;
    uint32_t arena_mb = 0;
    if (argc > 4 && !strcmp(argv[3], "--arena-mb")) {
        arena_mb = (uint32_t)strtoul(argv[4], NULL, 10);
        first = 5;
    }
    int rc = fs_open(&sim, argv[1], "flymeta.tns", arena_mb << 20, 0, NULL, NULL);
    if (rc != FS_OK) {
        fprintf(stderr, "fs_open: %s\n", fs_strerror(rc));
        return 1;
    }
    rc = app_init(&app, &sim, fb);
    if (rc != FS_OK) {
        fprintf(stderr, "app_init: %s\n", fs_strerror(rc));
        return 1;
    }
    char report[512] = "";
    int pass = app_selftest(&app, report, sizeof report, NULL, NULL) == SELFTEST_PASS;
    fputs(report, stdout);
    for (int i = first; i < argc; i++) {
        const char *c = argv[i];
        if (!strncmp(c, "f:", 2)) {
            for (long n = strtol(c + 2, NULL, 10); n > 0; n--) frame();
        } else if (!strncmp(c, "k:", 2)) {
            uint32_t bit = key_bit(c + 2);
            if (!bit) { fprintf(stderr, "unknown key %s\n", c + 2); return 2; }
            pending.pressed |= bit;
            frame();
        } else if (!strncmp(c, "t:", 2)) {
            pending.typed = c[2];
            frame();
        } else if (!strncmp(c, "s:", 2)) {
            if (save_ppm(argv[2], c + 2) != 0) { fprintf(stderr, "cannot write %s\n", c + 2); return 1; }
        } else {
            fprintf(stderr, "unknown command %s\n", c);
            return 2;
        }
    }
    printf("frames=%u step=%u spikes=%llu hash=%u selftest=%s\n", app.frame, sim.step, (unsigned long long)sim.total_spikes,
           sim.hash, pass ? "PASS" : "FAIL");
    app_free(&app);
    fs_close(&sim);
    return 0;
}
