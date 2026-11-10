/* Interactive front end shared by the calculator and the host preview. */
#ifndef APP_H
#define APP_H

#include <stdint.h>

#include "flysim.h"
#include "gfx.h"
#include "plat.h"

#define APP_MAX_GROUPS 32
#define APP_CUSTOM_CAP 4096u
#define APP_TOP_N 9
#define APP_MATCH_CAP 64
#define APP_QUERY_CAP 15
#define APP_MAX_SILENCED 8

/* Group indices fixed by pipeline/groups.py. */
enum {
    G_PROBOSCIS = 1, G_MOUTH, G_ANTENNA, G_NECK, G_LEG_FL, G_LEG_FR, G_LEG_ML, G_LEG_MR, G_LEG_HL, G_LEG_HR,
    G_WING_L, G_WING_R, G_FLIGHT, G_JUMP, G_HALTERE, G_ABDOMEN, G_MOTOR_MISC,
    G_PHOTO, G_OPTIC, G_VISPROJ, G_BRAIN, G_DESC, G_ASC, G_VNC, G_SENS_HEAD, G_SENS_BODY
};

enum { MODE_RUN, MODE_MENU, MODE_PICK, MODE_HELP };
enum { VIEW_FLY, VIEW_ACTIVITY, VIEW_SYSTEM, VIEW_COUNT };
enum { SIDE_BOTH, SIDE_LEFT, SIDE_RIGHT };
enum { SCN_NONE = -1, SCN_CUSTOM = -2 };

typedef struct {
    uint32_t scenario, steps, spikes, hash;
} app_selftest_t;

typedef struct {
    fs_sim_t *sim;
    gfx_t gfx;

    /* static tables from the meta file */
    const fs_group_t *groups;
    const fs_scenario_t *scen;
    const uint8_t *scen_base;
    const uint32_t *tnam_off;
    const char *tnam_chars;
    const uint16_t *tcnt;
    const uint32_t *tali_off; /* optional literature aliases (may be NULL) */
    const char *tali_chars;
    uint32_t n_groups, n_scen, n_types, map_w, map_h;

    /* activity map */
    uint8_t *glow, *gcol;
    uint16_t *bg;
    uint16_t ramp[APP_MAX_GROUPS][16];

    /* experiment control */
    int mode, view, scenario, side, paused, stim_on, quit;
    int tour;               /* auto-advance through the scenarios */
    uint32_t tour_stops;    /* stops completed; the first lap saves a snapshot of each */
    uint32_t tour_start_ms; /* wall time the current tour stop began */
    uint32_t tour_frame0;   /* frame counter when it began */
    uint32_t tick_hz;       /* rate of sim->ticks, if the platform set one (profiling) */
    uint32_t tour_step0;
    uint64_t tour_spikes0, tour_edges0;
    uint32_t rate_hz;
    uint32_t speed_pct; /* cap on simulated time per wall time, percent */
    uint32_t custom_ids[APP_CUSTOM_CAP];
    uint32_t custom_n, custom_type;
    uint32_t silenced[APP_MAX_SILENCED];
    uint32_t n_silenced;

    /* readouts */
    uint32_t prev_group[APP_MAX_GROUPS];
    uint32_t rate_x16[APP_MAX_GROUPS]; /* smoothed Hz * 16 per neuron */
    uint32_t top_type[APP_TOP_N], top_rate[APP_TOP_N], n_top;
    uint32_t top_since_step;
    uint32_t frame, anim;
    uint32_t steps_window, ms_window, window_start_ms, sim_permille; /* speed meter */
    uint32_t edges_per_sec;
    uint64_t edges_window_start;
    uint32_t render_ms; /* time to draw and present a frame, smoothed, x16 */

    /* modal state */
    int menu_sel, menu_top;
    int pick_silence, pick_sel, pick_top;
    char query[APP_QUERY_CAP + 1];
    uint32_t qlen, n_match;
    uint32_t match[APP_MATCH_CAP];

    /* boot self-check */
    int selftest; /* SELFTEST_* */
    uint32_t selftest_eps; /* edges per second measured */
} app_t;

int app_init(app_t *a, fs_sim_t *sim, uint16_t *fb);
void app_free(app_t *a);
/* Called every few steps of the self-check; return non-zero to abandon it. */
typedef int (*app_tick_fn)(void *ctx, uint32_t steps_done, uint32_t steps_total);
enum { SELFTEST_NOT_RUN, SELFTEST_PASS, SELFTEST_FAIL, SELFTEST_SKIPPED };
/* Run the boot self-check (bit-exact comparison with the host reference).
 * Returns one of the SELFTEST_* codes. */
int app_selftest(app_t *a, char *report, unsigned report_cap, app_tick_fn tick, void *ctx);
/* Handle input, advance the simulation for about one frame, render.
 * Returns 0 when the user quits. */
int app_frame(app_t *a, const plat_input_t *in);
/* Draw the run screen and present it (no input, no simulation). */
void app_render(app_t *a);

/* internals shared between the app_*.c files */
void app_select_scenario(app_t *a, int scenario);
void app_apply_stim(app_t *a);
void app_pick_type(app_t *a, uint32_t type, int silence);
void app_type_name(const app_t *a, uint32_t type, char *out, unsigned cap);
void app_type_alias(const app_t *a, uint32_t type, char *out, unsigned cap);
/* "alias:name" when the type has a literature alias, else just the name. */
void app_type_label(const app_t *a, uint32_t type, char *out, unsigned cap);
void app_draw_panel(app_t *a);
void app_draw_fly(app_t *a, int x0, int y0);
void app_modal_input(app_t *a, const plat_input_t *in);
void app_modal_draw(app_t *a);
void app_open_picker(app_t *a, int silence);

#endif
