/* TI-Nspire (Ndless) implementation of ui/plat.h. */
#include <libndls.h>
#include <stdio.h>
#include <string.h>

#include "plat.h"

#define LOG_NAME "flylog.tns"

/* Time.  The handheld has a real-time clock that counts seconds and SP804
 * dual timers at 0x900C0000 and 0x900D0000.  Timer 1 of the second block is
 * the OS tick (msleep() borrows it) and is never touched.  Of the others we
 * take the first one the OS has left disabled, run it free and measure its
 * rate against msleep().  If none counts, time falls back to the RTC: one
 * second resolution, which is enough for everything but smooth pacing. */
#define RTC_SECONDS (*(volatile uint32_t *)0x90090000)
enum { R_LOAD = 0, R_VALUE = 1, R_CONTROL = 2 };
#define SP804_ENABLED 0x80u
#define SP804_FREE_RUNNING_32BIT 0x82u /* enabled, 32-bit, no interrupt, no prescale */
#define CALIBRATION_MS 50u

static const struct {
    uint32_t addr;
    const char *name;
} TIMER_SLOTS[] = {{0x900C0020u, "C2"}, {0x900D0020u, "D2"}, {0x900C0000u, "C1"}};

static struct {
    volatile uint32_t *reg; /* chosen timer, or NULL: RTC fallback */
    uint32_t saved_load, saved_control;
    uint32_t ticks_per_s, last_value;
    uint64_t ticks;
    uint32_t rtc0;
    char report[200];
} tmr;

static uint8_t key_was_down[64];

/* Pick a clock source.  Returns its tick rate in Hz, or 0 for the RTC
 * fallback.  plat_timer_report() describes what was found. */
uint32_t plat_timer_init(void) {
    unsigned used = 0;
    tmr.reg = NULL;
    tmr.rtc0 = RTC_SECONDS;
    for (unsigned k = 0; k < sizeof TIMER_SLOTS / sizeof TIMER_SLOTS[0]; k++) {
        volatile uint32_t *reg = (volatile uint32_t *)TIMER_SLOTS[k].addr;
        uint32_t load = reg[R_LOAD], control = reg[R_CONTROL];
        unsigned long ticks = 0;
        if (!tmr.reg && !(control & SP804_ENABLED)) {
            reg[R_LOAD] = 0xFFFFFFFFu;
            reg[R_CONTROL] = SP804_FREE_RUNNING_32BIT;
            uint32_t before = reg[R_VALUE];
            msleep(CALIBRATION_MS);
            ticks = before - reg[R_VALUE];
            if (ticks >= 50u && ticks < 0x40000000u) {
                /* msleep() counts 32 ticks of the 32.768 kHz clock per "ms". */
                uint32_t rate = (uint32_t)(((uint64_t)ticks * 32768u) / (32u * CALIBRATION_MS));
                if (rate >= 29000u && rate <= 36000u) rate = 32768u;        /* nominal slow clock */
                if (rate >= 11400000u && rate <= 12600000u) rate = 12000000u; /* nominal fast clock */
                tmr.reg = reg;
                tmr.saved_load = load;
                tmr.saved_control = control;
                tmr.ticks_per_s = rate;
                tmr.ticks = 0;
                tmr.last_value = reg[R_VALUE];
            } else {
                reg[R_CONTROL] = control;
                reg[R_LOAD] = load;
            }
        }
        if (used < sizeof tmr.report) {
            int w = snprintf(tmr.report + used, sizeof tmr.report - used, " %s:ctl=%lx,load=%lx,ticks=%lu", TIMER_SLOTS[k].name,
                             (unsigned long)control, (unsigned long)load, ticks);
            if (w > 0) used += (unsigned)w;
        }
    }
    return tmr.reg ? tmr.ticks_per_s : 0;
}

const char *plat_timer_report(void) { return tmr.report; }

uint32_t plat_rtc(void) { return RTC_SECONDS; } /* seconds, independent of the timer above */

/* The chosen timer as an up-counter, for timing short stretches of code
 * (it wraps, so only differences mean anything).  Only valid if
 * plat_timer_init() found a timer. */
uint32_t plat_ticks(void) { return 0u - tmr.reg[R_VALUE]; }

void plat_timer_restore(void) {
    if (!tmr.reg) return;
    tmr.reg[R_CONTROL] = tmr.saved_control;
    tmr.reg[R_LOAD] = tmr.saved_load;
    tmr.reg = NULL;
}

uint32_t plat_ms(void) {
    if (!tmr.reg) return (RTC_SECONDS - tmr.rtc0) * 1000u;
    /* Accumulate deltas so a fast counter wrapping 32 bits does no harm. */
    uint32_t now = tmr.reg[R_VALUE];
    tmr.ticks += (uint32_t)(tmr.last_value - now);
    tmr.last_value = now;
    return (uint32_t)((tmr.ticks * 1000u) / tmr.ticks_per_s);
}

/* Frame pacing: spin on the fine clock when there is one, else msleep(). */
void plat_sleep(uint32_t ms) {
    if (ms > 200u) ms = 200u;
    if (tmr.reg) {
        uint32_t t0 = plat_ms();
        while (plat_ms() - t0 < ms) {
        }
    } else if (ms) {
        msleep(ms);
    }
}

/* Frames are already in the panel's native layout (240 x 320, one screen
 * column per line), so on the CX II this is a straight copy; on models
 * with a 320 x 240 panel Ndless converts. */
void plat_present(const uint16_t *fb) { lcd_blit((void *)fb, SCR_240x320_565); }

/* Raw little-endian RGB565 in that same layout: converted on the computer. */
void plat_snapshot(unsigned index, const uint16_t *fb) {
    char name[24];
    snprintf(name, sizeof name, "flyshot%02u.tns", index % 100u);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    size_t wrote = fwrite(fb, sizeof(uint16_t), 320u * 240u, f);
    fclose(f);
    if (wrote != 320u * 240u) {
        remove(name); /* a truncated picture is worse than none */
        plat_log("snapshot write failed");
    }
}

/* The whole log is kept in RAM and the file rewritten on every line, so the
 * file is complete after a crash and no append mode is needed. */
static char log_text[8192];
static unsigned log_len;

void plat_log(const char *line) {
    unsigned n = (unsigned)strlen(line);
    if (log_len + n + 2u > sizeof log_text) return; /* full: keep the head */
    memcpy(log_text + log_len, line, n);
    log_len += n;
    log_text[log_len++] = '\n';
    FILE *f = fopen(LOG_NAME, "wb");
    if (!f) return;
    fwrite(log_text, 1, log_len, f);
    fclose(f);
}

void plat_log_reset(void) { log_len = 0; }

static int edge(unsigned slot, int down) {
    int fresh = down && !key_was_down[slot];
    key_was_down[slot] = (uint8_t)(down != 0);
    return fresh;
}

void plat_poll(plat_input_t *in) {
    unsigned slot = 0;
    in->pressed = 0;
    in->typed = 0;
#define KEY(bit, cond)                                 \
    do {                                               \
        if (edge(slot++, (cond))) in->pressed |= (bit); \
    } while (0)
    KEY(PK_UP, isKeyPressed(KEY_NSPIRE_UP));
    KEY(PK_DOWN, isKeyPressed(KEY_NSPIRE_DOWN));
    KEY(PK_LEFT, isKeyPressed(KEY_NSPIRE_LEFT));
    KEY(PK_RIGHT, isKeyPressed(KEY_NSPIRE_RIGHT));
    KEY(PK_ENTER, isKeyPressed(KEY_NSPIRE_ENTER) || isKeyPressed(KEY_NSPIRE_CLICK) || isKeyPressed(KEY_NSPIRE_RET));
    KEY(PK_ESC, isKeyPressed(KEY_NSPIRE_ESC));
    KEY(PK_TAB, isKeyPressed(KEY_NSPIRE_TAB));
    KEY(PK_MENU, isKeyPressed(KEY_NSPIRE_MENU) || isKeyPressed(KEY_NSPIRE_DOC));
    KEY(PK_PLUS, isKeyPressed(KEY_NSPIRE_PLUS));
    KEY(PK_MINUS, isKeyPressed(KEY_NSPIRE_MINUS));
    KEY(PK_DEL, isKeyPressed(KEY_NSPIRE_DEL));
    KEY(PK_MUL, isKeyPressed(KEY_NSPIRE_MULTIPLY));
    KEY(PK_DIV, isKeyPressed(KEY_NSPIRE_DIVIDE));
#undef KEY
#define CHAR(ch, key)                                               \
    do {                                                            \
        if (edge(slot++, isKeyPressed(key)) && !in->typed) in->typed = (ch); \
    } while (0)
    CHAR('a', KEY_NSPIRE_A); CHAR('b', KEY_NSPIRE_B); CHAR('c', KEY_NSPIRE_C); CHAR('d', KEY_NSPIRE_D);
    CHAR('e', KEY_NSPIRE_E); CHAR('f', KEY_NSPIRE_F); CHAR('g', KEY_NSPIRE_G); CHAR('h', KEY_NSPIRE_H);
    CHAR('i', KEY_NSPIRE_I); CHAR('j', KEY_NSPIRE_J); CHAR('k', KEY_NSPIRE_K); CHAR('l', KEY_NSPIRE_L);
    CHAR('m', KEY_NSPIRE_M); CHAR('n', KEY_NSPIRE_N); CHAR('o', KEY_NSPIRE_O); CHAR('p', KEY_NSPIRE_P);
    CHAR('q', KEY_NSPIRE_Q); CHAR('r', KEY_NSPIRE_R); CHAR('s', KEY_NSPIRE_S); CHAR('t', KEY_NSPIRE_T);
    CHAR('u', KEY_NSPIRE_U); CHAR('v', KEY_NSPIRE_V); CHAR('w', KEY_NSPIRE_W); CHAR('x', KEY_NSPIRE_X);
    CHAR('y', KEY_NSPIRE_Y); CHAR('z', KEY_NSPIRE_Z);
    CHAR('0', KEY_NSPIRE_0); CHAR('1', KEY_NSPIRE_1); CHAR('2', KEY_NSPIRE_2); CHAR('3', KEY_NSPIRE_3);
    CHAR('4', KEY_NSPIRE_4); CHAR('5', KEY_NSPIRE_5); CHAR('6', KEY_NSPIRE_6); CHAR('7', KEY_NSPIRE_7);
    CHAR('8', KEY_NSPIRE_8); CHAR('9', KEY_NSPIRE_9);
    CHAR(' ', KEY_NSPIRE_SPACE); CHAR('_', KEY_NSPIRE_NEGATIVE); CHAR('(', KEY_NSPIRE_LP);
#undef CHAR
}
