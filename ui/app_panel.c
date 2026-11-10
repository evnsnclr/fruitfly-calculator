/* Right-hand panel: experiment header, fly, readout bars, status. */
#include <stdio.h>
#include <string.h>

#include "app.h"

#define PX 183 /* panel left edge */
#define PW 137
#define C_BG RGB565(8, 10, 18)
#define C_TEXT RGB565(220, 225, 235)
#define C_DIM RGB565(120, 130, 150)
#define C_TITLE RGB565(255, 220, 90)
#define C_ON RGB565(90, 235, 130)
#define C_OFF RGB565(255, 90, 70)
#define C_BAR_BG RGB565(30, 36, 54)
#define BAR_FULL_HZ 100u

static const char *const SIDE_NAME[] = {"L+R", "L", "R"};

static uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    for (; bit; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return r;
}

/* Square-root scale: pools differ a lot in size, so mean rates span two
 * orders of magnitude.  1 Hz is a tenth of the bar, 100 Hz fills it. */
static void bar(app_t *a, int x, int y, int w, uint32_t rate_x16, uint16_t color) {
    gfx_rect(&a->gfx, x, y, w, 6, C_BAR_BG);
    uint32_t fill = isqrt(rate_x16 * (uint32_t)w * (uint32_t)w / (BAR_FULL_HZ * 16u));
    if (fill > (uint32_t)w) fill = (uint32_t)w;
    if (fill) gfx_rect(&a->gfx, x, y, (int)fill, 6, color);
}

static void bar_row(app_t *a, int y, const char *label, int g_left, int g_right) {
    gfx_textn(&a->gfx, PX, y, label, 9, C_TEXT);
    const int bx = PX + 56, bw = PW - 58;
    if (g_right < 0) {
        bar(a, bx, y, bw, a->rate_x16[g_left], a->groups[g_left].rgb565);
    } else {
        int half = (bw - 2) / 2;
        bar(a, bx, y, half, a->rate_x16[g_left], a->groups[g_left].rgb565);
        bar(a, bx + half + 2, y, half, a->rate_x16[g_right], a->groups[g_right].rgb565);
    }
}

static void draw_header(app_t *a) {
    char line[40];
    const char *name = "No stimulus", *hint = "spontaneously silent";
    char custom[24], tname[16];
    uint32_t cells = 0;
    if (a->scenario >= 0) {
        const fs_scenario_t *sc = &a->scen[a->scenario];
        name = sc->name;
        hint = sc->hint;
        cells = a->side == SIDE_LEFT ? sc->n_left : a->side == SIDE_RIGHT ? sc->n_right : sc->n_ids;
    } else if (a->scenario == SCN_CUSTOM) {
        app_type_name(a, a->custom_type, tname, sizeof tname);
        snprintf(custom, sizeof custom, "Type %.15s", tname);
        name = custom;
        hint = "your pick: watch it spread";
        cells = a->custom_n;
    }
    /* name[20] and hint[28] come from the file and need not be terminated. */
    gfx_textn(&a->gfx, PX, 2, name, 19, C_TITLE);
    gfx_textn(&a->gfx, PX, 11, hint, 22, C_DIM);
    snprintf(line, sizeof line, "%lu cells %luHz %s", (unsigned long)cells, (unsigned long)a->rate_hz, SIDE_NAME[a->side]);
    gfx_text(&a->gfx, PX, 20, line, 19, C_TEXT);
    gfx_text(&a->gfx, PX + PW - 18, 20, a->stim_on ? " ON" : "OFF", 3, a->stim_on ? C_ON : C_OFF);
}

static void draw_status(app_t *a) {
    const fs_sim_t *s = a->sim;
    char line[40];
    uint32_t ms = (uint32_t)(((uint64_t)s->step * s->hdr->dt_us) / 1000u);
    snprintf(line, sizeof line, "t=%lu.%03lus  x%lu.%03lu%s", (unsigned long)(ms / 1000u), (unsigned long)(ms % 1000u),
             (unsigned long)(a->sim_permille / 1000u), (unsigned long)(a->sim_permille % 1000u), a->paused ? " ||" : "");
    gfx_text(&a->gfx, PX, 214, line, 22, C_TEXT);
    snprintf(line, sizeof line, "gain %lu.%02lumV %s", (unsigned long)(s->w_syn_uv / 1000u),
             (unsigned long)((s->w_syn_uv % 1000u) / 10u), s->tier[1].enabled ? "all syn" : "syn>=2");
    gfx_text(&a->gfx, PX, 223, line, 22, C_DIM);
    if (a->tour) {
        char tour[32];
        snprintf(tour, sizeof tour, "TOUR %d/%lu  key: manual", a->scenario + 1, (unsigned long)a->n_scen);
        gfx_text(&a->gfx, PX, 232, tour, 22, C_TITLE);
    }
    else gfx_text(&a->gfx, PX, 232, "menu  tab:view  H:help", 22, C_DIM);
}

static void view_fly(app_t *a) {
    app_draw_fly(a, PX + PW / 2, 74);
    int y = 122;
    bar_row(a, y, "Proboscis", G_PROBOSCIS, -1); y += 8;
    bar_row(a, y, "Mouth", G_MOUTH, -1); y += 8;
    bar_row(a, y, "Antenna", G_ANTENNA, -1); y += 8;
    bar_row(a, y, "Neck", G_NECK, -1); y += 8;
    bar_row(a, y, "Legs F", G_LEG_FL, G_LEG_FR); y += 8;
    bar_row(a, y, "Legs M", G_LEG_ML, G_LEG_MR); y += 8;
    bar_row(a, y, "Legs H", G_LEG_HL, G_LEG_HR); y += 8;
    bar_row(a, y, "Wings", G_WING_L, G_WING_R); y += 8;
    bar_row(a, y, "Flight", G_FLIGHT, -1); y += 8;
    bar_row(a, y, "Jump TTM", G_JUMP, -1); y += 8;
    bar_row(a, y, "Abdomen", G_ABDOMEN, -1);
}

static void view_activity(app_t *a) {
    static const int REGIONS[] = {G_SENS_HEAD, G_SENS_BODY, G_PHOTO, G_OPTIC, G_VISPROJ, G_BRAIN, G_DESC, G_ASC, G_VNC};
    int y = 31;
    for (unsigned k = 0; k < sizeof REGIONS / sizeof REGIONS[0]; k++, y += 9)
        bar_row(a, y, a->groups[REGIONS[k]].name, REGIONS[k], -1);
    y += 3;
    gfx_text(&a->gfx, PX, y, "Most active types   Hz", 22, C_TITLE);
    y += 10;
    char label[40], line[48];
    for (uint32_t k = 0; k < a->n_top; k++, y += 9) {
        app_type_label(a, a->top_type[k], label, sizeof label);
        snprintf(line, sizeof line, "%-18.18s%4lu", label, (unsigned long)a->top_rate[k]);
        gfx_text(&a->gfx, PX, y, line, 22, C_TEXT);
    }
    if (a->n_top == 0) gfx_text(&a->gfx, PX, y, "(nothing firing yet)", 22, C_DIM);
}

static void view_system(app_t *a) {
    const fs_sim_t *s = a->sim;
    const fs_header_t *h = s->hdr;
    char line[40];
    int y = 31;
#define ROW(...)                                               \
    do {                                                       \
        snprintf(line, sizeof line, __VA_ARGS__);              \
        gfx_text(&a->gfx, PX, y, line, 22, C_TEXT);            \
        y += 9;                                                \
    } while (0)
    gfx_text(&a->gfx, PX, y, "Male CNS connectome v1", 22, C_TITLE); y += 10;
    ROW("neurons  %lu", (unsigned long)h->n_neurons);
    ROW("synapses %lu", (unsigned long)(h->n_synapses[0] + h->n_synapses[1]));
    ROW("connect. %lu", (unsigned long)(h->n_edges[0] + h->n_edges[1]));
    ROW(" >=2 syn %lu", (unsigned long)h->n_edges[0]);
    ROW(" 1 syn   %lu", (unsigned long)h->n_edges[1]);
    y += 3;
    gfx_text(&a->gfx, PX, y, "This calculator", 22, C_TITLE); y += 10;
    ROW("in RAM %lu.%lu/%lu.%lu MB", (unsigned long)(s->resident_bytes >> 20), (unsigned long)(((s->resident_bytes & 0xFFFFFu) * 10u) >> 20),
        (unsigned long)(s->total_bytes >> 20), (unsigned long)(((s->total_bytes & 0xFFFFFu) * 10u) >> 20));
    ROW("paged in %lu flush %lu", (unsigned long)s->page_faults, (unsigned long)s->pool_flushes);
    if (s->io_errors) {
        snprintf(line, sizeof line, "READ ERRORS %lu", (unsigned long)s->io_errors);
        gfx_text(&a->gfx, PX, y, line, 22, C_OFF);
        y += 9;
    }
    ROW("syn events/s %lu", (unsigned long)a->edges_per_sec);
    ROW("awake now %lu", (unsigned long)s->n_awake);
    ROW("frame draw %lu ms", (unsigned long)(a->render_ms / 16u));
    ROW("spikes %lu", (unsigned long)s->total_spikes);
    ROW("step %lu.%lums thr %ld", (unsigned long)(h->dt_us / 1000u), (unsigned long)((h->dt_us % 1000u) / 100u), (long)(s->theta >> FS_FRAC));
    y += 3;
    if (a->selftest == SELFTEST_PASS) gfx_text(&a->gfx, PX, y, "Self-check: bit-exact", 22, C_ON);
    else if (a->selftest == SELFTEST_FAIL) gfx_text(&a->gfx, PX, y, "Self-check: MISMATCH", 22, C_OFF);
    else gfx_text(&a->gfx, PX, y, "Self-check: not run", 22, C_DIM);
    y += 9;
    if (a->selftest_eps) ROW("bench %lu syn ev/s", (unsigned long)a->selftest_eps);
    if (a->n_silenced) {
        char name[16];
        app_type_name(a, a->silenced[a->n_silenced - 1], name, sizeof name);
        ROW("silenced %lu: %s", (unsigned long)a->n_silenced, name);
    }
#undef ROW
}

void app_draw_panel(app_t *a) {
    gfx_rect(&a->gfx, (int)a->map_w, 0, GFX_W - (int)a->map_w, GFX_H_PX, C_BG);
    draw_header(a);
    switch (a->view) {
    case VIEW_ACTIVITY: view_activity(a); break;
    case VIEW_SYSTEM: view_system(a); break;
    default: view_fly(a); break;
    }
    draw_status(a);
}
