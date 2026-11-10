/* Main loop of the interactive demo: input, simulation pacing, activity map. */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_BUDGET_MS 90u
#define MAX_STEPS_PER_FRAME 200u
/* A frame also ends after this much work, so the display stays live even on
 * a platform whose clock only counts seconds. */
#define FRAME_EDGE_BUDGET 150000u
#define FRAME_FAULT_BUDGET 4u
#define SPEED_WINDOW_MS 3000u
#define GLOW_MAX 15u
#define RATE_MIN 5u
#define RATE_MAX 300u
#define GAIN_STEP_UV 5u
#define GAIN_MIN_UV 60u
#define GAIN_MAX_UV 300u
#define TOUR_STOP_MS 9000u
#define SELFTEST_MAX_TESTS 8u
#define SELFTEST_MAX_STEPS 5000u

static const uint32_t SPEEDS[] = {2, 5, 10, 25, 50, 100, 400};
#define N_SPEEDS (sizeof SPEEDS / sizeof SPEEDS[0])

/* ------------------------------------------------------------------ init */

static void build_ramps(app_t *a) {
    for (uint32_t g = 0; g < a->n_groups; g++) {
        uint16_t c = a->groups[g].rgb565;
        for (unsigned l = 0; l <= GLOW_MAX; l++) a->ramp[g][l] = gfx_scale(c, 3u + l * 4u / 5u, 15u);
    }
}

/* The data file numbers map pixels row by row; our buffers (like the
 * framebuffer) are column by column. */
static inline uint32_t map_index(const app_t *a, uint32_t pix) {
    return (pix % a->map_w) * a->map_h + pix / a->map_w;
}

static void build_background(app_t *a) {
    const uint32_t npix = a->map_w * a->map_h;
    uint8_t *density = calloc(npix, 1);
    if (!density) return; /* map stays black; purely cosmetic */
    for (uint32_t i = 0; i < a->sim->n; i++) {
        uint8_t *d = &density[map_index(a, a->sim->pix[i])];
        if (*d < 255) (*d)++;
    }
    for (uint32_t p = 0; p < npix; p++) {
        unsigned d = density[p];
        if (d == 0) {
            a->bg[p] = 0;
        } else {
            unsigned l = 5u + (d > 10u ? 10u : d);
            a->bg[p] = RGB565(l * 2u, l * 3u, 10u + l * 5u);
        }
    }
    free(density);
}

/* A name table is (n + 1) ascending uint32 offsets followed by the
 * characters.  Returns 0 (and leaves the outputs NULL) unless every name
 * lies inside the section. */
static int name_table(const uint8_t *sec, uint32_t size, uint32_t n, const uint32_t **off_out, const char **chars_out) {
    *off_out = NULL;
    *chars_out = NULL;
    uint64_t head = ((uint64_t)n + 1u) * 4u;
    if (!sec || head > size) return 0;
    const uint32_t *off = (const uint32_t *)(const void *)sec;
    if (off[0] != 0 || off[n] > size - head) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (off[i] > off[i + 1]) return 0;
    *off_out = off;
    *chars_out = (const char *)sec + head;
    return 1;
}

int app_init(app_t *a, fs_sim_t *sim, uint16_t *fb) {
    memset(a, 0, sizeof *a);
    a->sim = sim;
    a->gfx.px = fb;
    const fs_header_t *h = sim->hdr;
    uint32_t size = 0;
    a->n_groups = h->n_groups;
    a->n_scen = h->n_scenarios;
    a->n_types = h->n_types;
    a->map_w = h->map_w;
    a->map_h = h->map_h;
    a->groups = fs_section(sim, "GRPS", &size);
    /* The panel indexes groups up to G_SENS_BODY by constant. */
    if (!a->groups || a->n_groups > APP_MAX_GROUPS || a->n_groups <= G_SENS_BODY ||
        (uint64_t)a->n_groups * sizeof(fs_group_t) > size)
        return FS_ERR_FORMAT;
    a->scen = fs_section(sim, "SCEN", &size);
    a->scen_base = (const uint8_t *)a->scen;
    if (!a->scen || (uint64_t)a->n_scen * sizeof(fs_scenario_t) > size) return FS_ERR_FORMAT;
    for (uint32_t k = 0; k < a->n_scen; k++) {
        const fs_scenario_t *sc = &a->scen[k];
        if ((sc->ids_off & 3u) || (uint64_t)sc->ids_off + (uint64_t)sc->n_ids * 4u > size ||
            (uint64_t)sc->n_left + sc->n_right > sc->n_ids)
            return FS_ERR_FORMAT;
    }
    if (!name_table(fs_section(sim, "TNAM", &size), size, a->n_types, &a->tnam_off, &a->tnam_chars))
        return FS_ERR_FORMAT;
    /* Aliases are optional: a bad table just means no aliases. */
    (void)name_table(fs_section(sim, "TALI", &size), size, a->n_types, &a->tali_off, &a->tali_chars);
    a->tcnt = fs_section(sim, "TCNT", &size);
    if (!a->tcnt || size < a->n_types * 2u) return FS_ERR_FORMAT;
    if (a->map_w == 0 || a->map_w > 200u || a->map_h != GFX_H_PX) return FS_ERR_FORMAT;

    const uint32_t npix = a->map_w * a->map_h;
    for (uint32_t i = 0; i < sim->n; i++)
        if (sim->pix[i] >= npix) return FS_ERR_FORMAT;
    a->glow = calloc(npix, 1);
    a->gcol = calloc(npix, 1);
    a->bg = calloc(npix, sizeof(uint16_t));
    if (!a->glow || !a->gcol || !a->bg) {
        app_free(a);
        return FS_ERR_MEMORY;
    }
    build_ramps(a);
    build_background(a);
    a->speed_pct = 100; /* as fast as the machine manages, but never faster than the fly */
    a->stim_on = 1;
    a->tour = 1; /* until the first key press */
    a->window_start_ms = plat_ms();
    app_select_scenario(a, 0);
    return FS_OK;
}

void app_free(app_t *a) {
    free(a->glow);
    free(a->gcol);
    free(a->bg);
    a->glow = a->gcol = NULL;
    a->bg = NULL;
}

/* ------------------------------------------------------------- stimulus */

void app_type_name(const app_t *a, uint32_t type, char *out, unsigned cap) {
    if (cap == 0) return;
    if (type >= a->n_types) {
        out[0] = 0;
        return;
    }
    uint32_t len = a->tnam_off[type + 1] - a->tnam_off[type];
    if (len >= cap) len = cap - 1u;
    memcpy(out, a->tnam_chars + a->tnam_off[type], len);
    out[len] = 0;
}

void app_type_alias(const app_t *a, uint32_t type, char *out, unsigned cap) {
    if (cap == 0) return;
    out[0] = 0;
    if (!a->tali_off || type >= a->n_types) return;
    uint32_t len = a->tali_off[type + 1] - a->tali_off[type];
    if (len >= cap) len = cap - 1u;
    memcpy(out, a->tali_chars + a->tali_off[type], len);
    out[len] = 0;
}

void app_type_label(const app_t *a, uint32_t type, char *out, unsigned cap) {
    char name[32], alias[16];
    app_type_name(a, type, name, sizeof name);
    app_type_alias(a, type, alias, sizeof alias);
    if (alias[0]) snprintf(out, cap, "%s:%s", alias, name);
    else snprintf(out, cap, "%s", name);
}

void app_apply_stim(app_t *a) {
    fs_sim_t *s = a->sim;
    fs_clear_stims(s);
    if (!a->stim_on) return;
    if (a->scenario >= 0) {
        const fs_scenario_t *sc = &a->scen[a->scenario];
        const uint32_t *ids = (const uint32_t *)(const void *)(a->scen_base + sc->ids_off);
        uint32_t n = sc->n_ids;
        if (a->side == SIDE_LEFT) n = sc->n_left;
        if (a->side == SIDE_RIGHT) {
            ids += sc->n_left;
            n = sc->n_right;
        }
        fs_set_stim(s, 0, ids, n, a->rate_hz);
    } else if (a->scenario == SCN_CUSTOM) {
        fs_set_stim(s, 0, a->custom_ids, a->custom_n, a->rate_hz);
    }
}

static void reset_readouts(app_t *a) {
    memset(a->prev_group, 0, sizeof a->prev_group);
    memset(a->rate_x16, 0, sizeof a->rate_x16);
    a->n_top = 0;
    a->top_since_step = a->sim->step;
    fs_clear_counts(a->sim);
}

static void tour_mark(app_t *a) {
    a->tour_start_ms = plat_ms();
    a->tour_frame0 = a->frame;
    a->tour_step0 = 0; /* app_select_scenario resets the simulation clock */
    a->tour_spikes0 = 0;
    a->tour_edges0 = 0;
}

/* One log line per tour stop: the calculator's real throughput per scenario. */
static void tour_log(const app_t *a, uint32_t now) {
    const fs_sim_t *s = a->sim;
    char line[280];
    uint32_t ms = now - a->tour_start_ms;
    /* Only the first lap is logged: later laps would just fill the log. */
    if (a->scenario < 0 || ms == 0 || a->tour_stops >= a->n_scen) return;
    snprintf(line, sizeof line,
             "tour scn=%d ms=%lu steps=%lu spikes=%lu edges=%lu eps=%lu faults=%lu promo=%lu gens=%lu ioerr=%lu render_ms=%lu"
             " awake=%lu stepped=%lu replayed=%lu",
             a->scenario, (unsigned long)ms, (unsigned long)s->step, (unsigned long)s->total_spikes,
             (unsigned long)s->total_edges, (unsigned long)((s->total_edges * 1000u) / ms), (unsigned long)s->page_faults,
             (unsigned long)s->promotions, (unsigned long)s->pool_flushes, (unsigned long)s->io_errors,
             (unsigned long)(a->render_ms / 16u), (unsigned long)s->n_awake, (unsigned long)s->awake_updates,
             (unsigned long)s->catchup_steps);
    plat_log(line);
    if (s->ticks && a->tick_hz) {
        /* Where the time went, from the core's own stopwatch. */
        const uint64_t hz = a->tick_hz;
        snprintf(line, sizeof line, "prof scn=%d frames=%lu deliver_ms=%lu of_which_flash_ms=%lu stim_integrate_ms=%lu",
                 a->scenario, (unsigned long)(a->frame - a->tour_frame0), (unsigned long)(s->ticks_deliver * 1000u / hz),
                 (unsigned long)(s->ticks_fault * 1000u / hz), (unsigned long)(s->ticks_integrate * 1000u / hz));
        plat_log(line);
    }
}

void app_select_scenario(app_t *a, int scenario) {
    if (scenario >= (int)a->n_scen) scenario = SCN_NONE;
    a->scenario = scenario;
    if (scenario >= 0) a->rate_hz = a->scen[scenario].rate_hz;
    fs_reset(a->sim);
    memset(a->glow, 0, a->map_w * a->map_h);
    reset_readouts(a);
    a->stim_on = 1;
    app_apply_stim(a);
    tour_mark(a);
}

static void rebuild_custom(app_t *a) {
    const fs_sim_t *s = a->sim;
    a->custom_n = 0;
    for (uint32_t i = 0; i < s->n && a->custom_n < APP_CUSTOM_CAP; i++) {
        if (s->type[i] != a->custom_type) continue;
        uint32_t side = (s->flags[i] >> FS_SIDE_SHIFT) & FS_SIDE_MASK;
        if ((a->side == SIDE_LEFT && side != 1u) || (a->side == SIDE_RIGHT && side != 2u)) continue;
        a->custom_ids[a->custom_n++] = i;
    }
}

static void apply_silencing(app_t *a) {
    fs_sim_t *s = a->sim;
    for (uint32_t i = 0; i < s->n; i++) {
        int off = 0;
        for (uint32_t k = 0; k < a->n_silenced; k++)
            if (s->type[i] == a->silenced[k]) off = 1;
        fs_set_silenced(s, i, off);
    }
}

void app_pick_type(app_t *a, uint32_t type, int silence) {
    if (type >= a->n_types) return;
    if (silence) {
        uint32_t k;
        for (k = 0; k < a->n_silenced; k++)
            if (a->silenced[k] == type) break;
        if (k < a->n_silenced) {
            a->silenced[k] = a->silenced[--a->n_silenced]; /* toggle off */
        } else if (a->n_silenced < APP_MAX_SILENCED) {
            a->silenced[a->n_silenced++] = type;
        }
        apply_silencing(a);
        return;
    }
    a->custom_type = type;
    a->rate_hz = 100;
    a->scenario = SCN_CUSTOM;
    rebuild_custom(a);
    fs_reset(a->sim);
    memset(a->glow, 0, a->map_w * a->map_h);
    reset_readouts(a);
    a->stim_on = 1;
    app_apply_stim(a);
}

/* ------------------------------------------------------------ self-check */

int app_selftest(app_t *a, char *report, unsigned report_cap, app_tick_fn tick, void *ctx) {
    uint32_t size = 0;
    const app_selftest_t *t = fs_section(a->sim, "TEST", &size);
    uint32_t n = t ? size / (uint32_t)sizeof *t : 0;
    fs_sim_t *s = a->sim;
    if (n > SELFTEST_MAX_TESTS) n = SELFTEST_MAX_TESTS;
    /* No tests in the data file means nothing was compared, not a failure. */
    int result = n > 0 ? SELFTEST_PASS : SELFTEST_SKIPPED;
    int abandoned = 0;
    uint32_t total_steps = 0, done_steps = 0;
    for (uint32_t k = 0; k < n; k++)
        if (t[k].steps <= SELFTEST_MAX_STEPS) total_steps += t[k].steps;
    uint32_t t0 = plat_ms();
    uint64_t edges = 0;
    unsigned used = 0;
    for (uint32_t k = 0; k < n && !abandoned; k++) {
        /* The step count comes from the data file: never let it stall boot. */
        if (t[k].scenario >= a->n_scen || t[k].steps > SELFTEST_MAX_STEPS) {
            result = SELFTEST_FAIL;
            continue;
        }
        const fs_scenario_t *sc = &a->scen[t[k].scenario];
        fs_reset(s);
        fs_clear_stims(s);
        fs_set_gain(s, s->hdr->w_syn_uv);
        fs_set_stim(s, 0, (const uint32_t *)(const void *)(a->scen_base + sc->ids_off), sc->n_ids, sc->rate_hz);
        s->io_errors = 0; /* judge this test on its own reads */
        uint32_t i = 0;
        for (; i < t[k].steps; i++) {
            if (tick && (i & 3u) == 0 && tick(ctx, done_steps + i, total_steps)) break;
            fs_step(s);
        }
        edges += s->total_edges;
        if (i < t[k].steps) {
            abandoned = 1; /* an earlier mismatch still stands */
            if (result != SELFTEST_FAIL) result = SELFTEST_SKIPPED;
            break;
        }
        done_steps += t[k].steps;
        int ok = s->hash == t[k].hash && s->total_spikes == t[k].spikes && s->io_errors == 0;
        if (!ok) result = SELFTEST_FAIL;
        if (report && used < report_cap) {
            int w = snprintf(report + used, report_cap - used, "test%u scn=%u steps=%u spikes=%lu/%lu hash=%08lx/%08lx %s\n",
                             (unsigned)k, (unsigned)t[k].scenario, (unsigned)t[k].steps, (unsigned long)s->total_spikes,
                             (unsigned long)t[k].spikes, (unsigned long)s->hash, (unsigned long)t[k].hash,
                             ok ? "OK" : "MISMATCH");
            if (w > 0) used += (unsigned)w < report_cap - used ? (unsigned)w : report_cap - used - 1u;
        }
    }
    static const char *const NAMES[] = {"NOT-RUN", "PASS", "FAIL", "SKIPPED"};
    uint32_t ms = plat_ms() - t0;
    a->selftest = result;
    a->selftest_eps = ms ? (uint32_t)(edges * 1000u / ms) : 0;
    if (report && used < report_cap)
        snprintf(report + used, report_cap - used, "selftest %s edges=%lu ms=%lu eps=%lu faults=%lu flushes=%lu\n",
                 NAMES[result], (unsigned long)edges, (unsigned long)ms, (unsigned long)a->selftest_eps,
                 (unsigned long)s->page_faults, (unsigned long)s->pool_flushes);
    app_select_scenario(a, 0);
    return result;
}

/* ------------------------------------------------------------------ input */

static void change_speed(app_t *a, int dir) {
    unsigned k = 0;
    while (k + 1 < N_SPEEDS && SPEEDS[k] < a->speed_pct) k++;
    if (dir > 0 && k + 1 < N_SPEEDS) k++;
    if (dir < 0 && k > 0) k--;
    a->speed_pct = SPEEDS[k];
}

static void run_input(app_t *a, const plat_input_t *in) {
    fs_sim_t *s = a->sim;
    uint32_t p = in->pressed;
    if (p || in->typed) a->tour = 0; /* any key: the user takes over */
    if (p & PK_ESC) a->quit = 1;
    if (p & PK_MENU) {
        a->mode = MODE_MENU;
        a->menu_sel = a->scenario >= 0 ? a->scenario : 0;
    }
    if (p & PK_TAB) a->view = (a->view + 1) % VIEW_COUNT;
    if (p & PK_DOWN) app_select_scenario(a, a->scenario + 1 >= (int)a->n_scen || a->scenario < 0 ? 0 : a->scenario + 1);
    if (p & PK_UP) app_select_scenario(a, a->scenario <= 0 ? (int)a->n_scen - 1 : a->scenario - 1);
    if (p & (PK_LEFT | PK_RIGHT)) {
        uint32_t step = a->rate_hz >= 100u ? 20u : 10u;
        if ((p & PK_RIGHT) && a->rate_hz + step <= RATE_MAX) a->rate_hz += step;
        if ((p & PK_LEFT) && a->rate_hz >= RATE_MIN + step) a->rate_hz -= step;
        app_apply_stim(a);
    }
    if (p & PK_ENTER) {
        a->stim_on = !a->stim_on;
        app_apply_stim(a);
    }
    if ((p & PK_PLUS) && s->w_syn_uv + GAIN_STEP_UV <= GAIN_MAX_UV) fs_set_gain(s, s->w_syn_uv + GAIN_STEP_UV);
    if ((p & PK_MINUS) && s->w_syn_uv >= GAIN_MIN_UV + GAIN_STEP_UV) fs_set_gain(s, s->w_syn_uv - GAIN_STEP_UV);
    if (p & PK_MUL) change_speed(a, 1);
    if (p & PK_DIV) change_speed(a, -1);
    switch (in->typed) {
    case 'p': a->paused = !a->paused; break;
    case 'r':
        fs_reset(s);
        memset(a->glow, 0, a->map_w * a->map_h);
        reset_readouts(a);
        break;
    case 's':
        a->side = (a->side + 1) % 3;
        if (a->scenario == SCN_CUSTOM) rebuild_custom(a);
        app_apply_stim(a);
        break;
    case 'w':
        if (s->hdr->n_tiers > 1) s->tier[1].enabled = !s->tier[1].enabled;
        break;
    case 't': app_open_picker(a, 0); break;
    case 'x': app_open_picker(a, 1); break;
    case 'h': a->mode = MODE_HELP; break;
    case 'a':
        a->tour = 1;
        tour_mark(a);
        break;
    default: break;
    }
}

/* -------------------------------------------------------------- readouts */

static void update_rates(app_t *a, uint32_t steps) {
    const fs_sim_t *s = a->sim;
    if (steps == 0) return;
    for (uint32_t g = 0; g < a->n_groups; g++) {
        uint32_t d = s->group_count[g] - a->prev_group[g];
        a->prev_group[g] = s->group_count[g];
        uint32_t size = a->groups[g].size ? a->groups[g].size : 1u;
        /* Hz * 16 = spikes * 16e6 / (neurons * steps * dt_us) */
        uint32_t inst = (uint32_t)(((uint64_t)d * 16000000u) / ((uint64_t)size * steps * s->hdr->dt_us));
        a->rate_x16[g] = (a->rate_x16[g] * 3u + inst) / 4u;
    }
}

/* Every ~200 simulated ms: rank cell types by mean firing rate. */
static void update_top_types(app_t *a) {
    fs_sim_t *s = a->sim;
    uint32_t span = s->step - a->top_since_step;
    if (span < 200000u / s->hdr->dt_us) return;
    a->n_top = 0;
    for (uint32_t t = 1; t < a->n_types; t++) {
        uint32_t c = s->type_count[t];
        if (c == 0) continue;
        uint32_t cells = a->tcnt[t] ? a->tcnt[t] : 1u;
        uint32_t rate = (uint32_t)(((uint64_t)c * 1000000u) / ((uint64_t)cells * span * s->hdr->dt_us));
        uint32_t k = a->n_top;
        if (k == APP_TOP_N && rate <= a->top_rate[k - 1]) continue;
        if (k < APP_TOP_N) a->n_top++; else k--;
        while (k > 0 && a->top_rate[k - 1] < rate) {
            a->top_rate[k] = a->top_rate[k - 1];
            a->top_type[k] = a->top_type[k - 1];
            k--;
        }
        a->top_rate[k] = rate;
        a->top_type[k] = t;
    }
    memset(s->type_count, 0, (size_t)a->n_types * sizeof(uint32_t));
    a->top_since_step = s->step;
}

static void update_speed(app_t *a, uint32_t steps, uint32_t now) {
    const fs_sim_t *s = a->sim;
    a->steps_window += steps;
    uint32_t el = now - a->window_start_ms;
    if (el < SPEED_WINDOW_MS) return;
    /* simulated ms per wall second, as per-mille of real time */
    a->sim_permille = (uint32_t)(((uint64_t)a->steps_window * s->hdr->dt_us) / el);
    a->edges_per_sec = (uint32_t)(((s->total_edges - a->edges_window_start) * 1000u) / el);
    a->edges_window_start = s->total_edges;
    a->steps_window = 0;
    a->window_start_ms = now;
}

/* ------------------------------------------------------------------- map */

static void light_spikes(app_t *a) {
    const fs_sim_t *s = a->sim;
    for (uint32_t k = 0; k < s->n_listed; k++) {
        uint32_t i = s->spikes[k], p = map_index(a, s->pix[i]);
        a->glow[p] = GLOW_MAX;
        a->gcol[p] = s->group[i];
    }
}

/* The map occupies screen columns 0..map_w-1, which in the column-major
 * framebuffer are simply its first map_w * map_h pixels: one linear pass. */
static void draw_map(app_t *a) {
    const uint32_t npix = a->map_w * a->map_h;
    uint16_t *dst = a->gfx.px;
    uint8_t *gl = a->glow;
    const uint8_t *gc = a->gcol;
    const uint16_t *bg = a->bg;
    for (uint32_t p = 0; p < npix; p++) {
        unsigned l = gl[p];
        if (l) {
            dst[p] = a->ramp[gc[p]][l];
            gl[p] = (uint8_t)(l - 1u);
        } else {
            dst[p] = bg[p];
        }
    }
}

void app_render(app_t *a) {
    draw_map(a);
    app_draw_panel(a);
    plat_present(a->gfx.px);
}

/* ----------------------------------------------------------------- frame */

int app_frame(app_t *a, const plat_input_t *in) {
    fs_sim_t *s = a->sim;
    if (a->mode != MODE_RUN) {
        app_modal_input(a, in);
        if (a->mode != MODE_RUN) {
            app_modal_draw(a);
            plat_present(a->gfx.px);
            plat_sleep(30);
            a->window_start_ms = plat_ms();
            a->steps_window = 0;
            a->edges_window_start = s->total_edges;
            return !a->quit;
        }
    } else {
        run_input(a, in);
    }
    if (a->quit) return 0;

    uint32_t t0 = plat_ms(), steps = 0;
    if (a->tour && t0 - a->tour_start_ms >= TOUR_STOP_MS) {
        tour_log(a, t0);
        /* First lap: keep the last frame of every experiment as evidence. */
        if (a->tour_stops < a->n_scen && a->scenario >= 0) plat_snapshot((unsigned)a->scenario, a->gfx.px);
        a->tour_stops++;
        app_select_scenario(a, a->scenario + 1 >= (int)a->n_scen || a->scenario < 0 ? 0 : a->scenario + 1);
    }
    if (!a->paused && a->mode == MODE_RUN) {
        /* Cap simulated time at speed_pct of wall time, at least one step. */
        uint32_t cap = (uint32_t)(((uint64_t)FRAME_BUDGET_MS * 1000u * a->speed_pct) / (100u * s->hdr->dt_us));
        if (cap < 1u) cap = 1u;
        if (cap > MAX_STEPS_PER_FRAME) cap = MAX_STEPS_PER_FRAME;
        const uint64_t edges0 = s->total_edges;
        const uint32_t faults0 = s->page_faults;
        do {
            fs_step(s);
            light_spikes(a);
            steps++;
        } while (steps < cap && plat_ms() - t0 < FRAME_BUDGET_MS && s->total_edges - edges0 < FRAME_EDGE_BUDGET &&
                 s->page_faults - faults0 < FRAME_FAULT_BUDGET);
    }
    update_rates(a, steps);
    update_top_types(a);
    uint32_t r0 = plat_ms();
    app_render(a);
    a->render_ms = (a->render_ms * 7u + (plat_ms() - r0) * 16u) / 8u; /* smoothed, x16 */
    a->frame++;
    if (!a->paused) a->anim++;
    /* Light scenarios finish their steps early: wait out the frame so the
     * speed cap really is a cap. */
    uint32_t spent = plat_ms() - t0;
    if (spent < FRAME_BUDGET_MS) plat_sleep(FRAME_BUDGET_MS - spent);
    update_speed(a, steps, plat_ms());
    return 1;
}
