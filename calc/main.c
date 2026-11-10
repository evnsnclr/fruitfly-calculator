/*
 * Drosophila male CNS connectome on a TI-Nspire CX II (Ndless).
 *
 * Boot: load the connectome into RAM (demand-paging what does not fit),
 * prove the simulation is bit-exact against the host reference, then run the
 * interactive demo.  Every stage is appended to flylog.tns so a failed or
 * slow start can be diagnosed from the computer.
 */
#include <libndls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

#define VERSION_TEXT "flycns 1.6"
/* RAM left for the OS after sizing the arena.  Builds up to 1.5 left 2 MB
 * and never saw less than 2.4 MB actually free afterwards; edge data that
 * is not in RAM costs a flash read each time, so most of the difference
 * goes to that.  The UI's buffers (about 0.3 MB) and the optional
 * benchmark's scratch (0.25 MB) also come out of what is left. */
#define OS_RESERVE_BYTES (5u << 18)
/* The self-check is a few seconds of work; give up rather than stall boot. */
#define SELFTEST_BUDGET_MS 60000u
#define FAULT_PROBES 32u /* per order: file order, reverse, scattered */

int bench_requested(void);
void bench_run(fs_sim_t *s, app_t *a);
uint32_t plat_timer_init(void);
const char *plat_timer_report(void);
uint32_t plat_rtc(void);
uint32_t plat_ticks(void);
void plat_timer_restore(void);
void plat_log_reset(void);

static uint16_t fb[GFX_W * GFX_H_PX];
static fs_sim_t sim;
static app_t app;
static gfx_t boot_gfx = {fb};
static char memory_line[64];

#define C_BG RGB565(8, 10, 18)
#define C_TEXT RGB565(220, 225, 235)
#define C_DIM RGB565(120, 130, 150)
#define C_TITLE RGB565(255, 220, 90)
#define C_BAR RGB565(90, 235, 130)
#define C_BAD RGB565(255, 90, 70)

static void logf_line(const char *fmt, unsigned long a, unsigned long b, unsigned long c) {
    char line[160];
    snprintf(line, sizeof line, fmt, a, b, c);
    plat_log(line);
}

static void boot_screen(const char *status, uint32_t done, uint32_t total, uint16_t status_color) {
    gfx_clear(&boot_gfx, C_BG);
    gfx_text(&boot_gfx, 20, 24, "Drosophila melanogaster", -1, C_TITLE);
    gfx_text(&boot_gfx, 20, 36, "male central nervous system", -1, C_TITLE);
    gfx_text(&boot_gfx, 20, 58, "166,700 neurons", -1, C_TEXT);
    gfx_text(&boot_gfx, 20, 70, "25,582,938 connections", -1, C_TEXT);
    gfx_text(&boot_gfx, 20, 82, "124,177,617 synapses", -1, C_TEXT);
    gfx_text(&boot_gfx, 20, 100, "Janelia FlyEM male CNS connectome v1.0", -1, C_DIM);
    gfx_text(&boot_gfx, 20, 112, "spiking model after Shiu et al. 2024", -1, C_DIM);
    gfx_text(&boot_gfx, 20, 136, memory_line, -1, C_TEXT);
    gfx_text(&boot_gfx, 20, 160, status, -1, status_color);
    gfx_frame(&boot_gfx, 20, 176, 280, 12, C_DIM);
    if (total) gfx_rect(&boot_gfx, 22, 178, (int)(((uint64_t)done * 276u) / total), 8, C_BAR);
    plat_present(fb);
}

static void on_progress(void *ctx, uint32_t done, uint32_t total) {
    char status[64];
    (void)ctx;
    snprintf(status, sizeof status, "Loading synapses into RAM  %lu / %lu MB", (unsigned long)(done >> 20),
             (unsigned long)(total >> 20));
    boot_screen(status, done, total, C_TEXT);
}

static void fail(const char *what, int rc) {
    char status[80];
    snprintf(status, sizeof status, "%s: %s", what, fs_strerror(rc));
    plat_log(status);
    boot_screen(status, 0, 0, C_BAD);
    gfx_text(&boot_gfx, 20, 200, "Needs flymeta, flycore, flyweak (.tns)", -1, C_DIM);
    gfx_text(&boot_gfx, 20, 212, "in this folder.  Press any key.", -1, C_DIM);
    plat_present(fb);
    wait_no_key_pressed();
    wait_key_pressed();
}

/* How much memory is still obtainable in small pieces (the arena wants 256 KB
 * ones)?  Tells us how much of the "free" RAM fragmentation costs. */
static unsigned long spare_blocks(void) {
    static void *held[512];
    unsigned long n = 0;
    while (n < 512u && (held[n] = malloc(64u << 10)) != NULL) n++;
    for (unsigned long k = 0; k < n; k++) free(held[k]);
    return n;
}

/* How long does one page-in from flash take?  Read up to FAULT_PROBES
 * blocks that are not resident in each of three orders: ascending through
 * the files, descending, and scattered.  The simulation asks for blocks in
 * no particular order, so if seeking backwards is slow that is what it
 * pays.  Fills in milliseconds per block and the number of blocks read for
 * each order (a block read once sits in the paging pool and is not a
 * page-in again, so later orders may find fewer). */
static void probe_fault_ms(uint32_t ms_per_block[3], uint32_t probes[3]) {
    static uint32_t tgt[4096], wgt[4096];
    const uint32_t nb = sim.n_blocks, units = nb * sim.hdr->n_tiers;
    const uint32_t stride = units / (3u * FAULT_PROBES) + 2u;
    for (unsigned order = 0; order < 3u; order++) {
        uint32_t x = 0x9E3779B9u, t0 = plat_ms();
        probes[order] = 0;
        for (uint32_t k = 0; k < units && probes[order] < FAULT_PROBES; k++) {
            uint32_t u = k * stride; /* file order */
            if (order == 1u) {
                u += stride / 2u;
                if (u >= units) break;
                u = units - 1u - u; /* from the end */
            } else if (order == 2u) {
                x = x * 1664525u + 1013904223u;
                u = (x >> 8) % units;
            }
            if (u >= units) break;
            const unsigned tier = u / nb;
            const uint32_t blk = u % nb;
            if (sim.tier[tier].where[blk] != FS_AT_FLASH) continue;
            (void)fs_decode_row(&sim, tier, blk << FS_BLOCK_SHIFT, tgt, wgt, 4096);
            probes[order]++;
        }
        ms_per_block[order] = probes[order] ? (plat_ms() - t0) / probes[order] : 0;
    }
}

typedef struct {
    uint32_t start_ms, last_draw_ms;
    int reason; /* 0 running, 1 esc, 2 too slow */
} selftest_ui_t;

static int selftest_tick(void *ctx, uint32_t done, uint32_t total) {
    selftest_ui_t *ui = ctx;
    uint32_t now = plat_ms();
    /* Redraw at most ten times a second: the screen must not dominate the
     * throughput this check also measures. */
    if (now - ui->last_draw_ms >= 100u || done == 0) {
        char status[64];
        snprintf(status, sizeof status, "Self-check: step %lu / %lu   esc skips", (unsigned long)done, (unsigned long)total);
        boot_screen(status, done, total, C_TEXT);
        ui->last_draw_ms = plat_ms();
    }
    if (isKeyPressed(KEY_NSPIRE_ESC)) ui->reason = 1;
    else if (now - ui->start_ms > SELFTEST_BUDGET_MS) ui->reason = 2;
    return ui->reason;
}

int main(int argc, char **argv) {
    (void)argc;
    assert_ndless_rev(2004); /* lcd_blit API */
    enable_relative_paths(argv);
    if (!lcd_init(SCR_240x320_565)) return 1;
    plat_log_reset();
    plat_log(VERSION_TEXT " boot");
    uint32_t hz = plat_timer_init();
    uint32_t boot_ms = plat_ms(), boot_rtc = plat_rtc();
    logf_line("timer_hz=%lu cx2=%lu ndless_rev=%lu", hz, (unsigned long)is_cx2, (unsigned long)nl_ndless_rev());
    plat_log(plat_timer_report());

    boot_screen("Opening connectome...", 0, 0, C_TEXT);
    uint32_t t0 = plat_ms();
    int rc = fs_open(&sim, "", "flymeta.tns", 0, OS_RESERVE_BYTES, on_progress, NULL);
    if (rc != FS_OK) {
        fail("Cannot load connectome", rc);
        goto out;
    }
    uint32_t load_ms = plat_ms() - t0;
    logf_line("loaded ms=%lu chunks=%lu pool_first=%lu", load_ms, sim.n_chunks, sim.pool_first);
    logf_line("resident=%lu total=%lu malloc_ran_dry=%lu", sim.resident_bytes, sim.total_bytes,
              (unsigned long)sim.arena_exhausted);
    snprintf(memory_line, sizeof memory_line, "RAM %lu.%lu of %lu.%lu MB (%lu+%lu chunks) %lus",
             (unsigned long)(sim.resident_bytes >> 20), (unsigned long)(((sim.resident_bytes & 0xFFFFFu) * 10u) >> 20),
             (unsigned long)(sim.total_bytes >> 20), (unsigned long)(((sim.total_bytes & 0xFFFFFu) * 10u) >> 20),
             (unsigned long)sim.pool_first, (unsigned long)(sim.n_chunks - sim.pool_first), (unsigned long)(load_ms / 1000u));

    rc = app_init(&app, &sim, fb);
    if (rc != FS_OK) {
        fail("Cannot start", rc);
        goto close_sim;
    }
    logf_line("spare_64k_blocks=%lu clock=%lu", spare_blocks(), (unsigned long)(hz ? 1 : 0), 0);
    if (bench_requested()) {
        boot_screen("Measuring this calculator...", 0, 0, C_TEXT);
        bench_run(&sim, &app);
    }
    boot_screen("Timing flash page-in...", 0, 0, C_TEXT);
    uint32_t fault_ms[3], fault_n[3];
    probe_fault_ms(fault_ms, fault_n);
    logf_line("fault_ms forward=%lu backward=%lu scattered=%lu", fault_ms[0], fault_ms[1], fault_ms[2]);
    logf_line("fault_probes forward=%lu backward=%lu scattered=%lu", fault_n[0], fault_n[1], fault_n[2]);
    logf_line("faults=%lu flushes=%lu", sim.page_faults, sim.pool_flushes, 0);
    if (hz) { /* from here on the core times its own phases */
        sim.ticks = plat_ticks;
        app.tick_hz = hz;
    }

    char report[400];
    selftest_ui_t ui = {plat_ms(), 0, 0};
    int result = app_selftest(&app, report, sizeof report, selftest_tick, &ui);
    plat_log(report);
    logf_line("selftest=%lu skip_reason=%lu ioerr=%lu", (unsigned long)result, (unsigned long)ui.reason, sim.io_errors);
    const char *verdict = result == SELFTEST_PASS   ? "Self-check passed: spike trains bit-exact"
                          : result == SELFTEST_FAIL ? "Self-check FAILED (see flylog)"
                          : ui.reason == 2          ? "Self-check skipped: too slow (see flylog)"
                          : ui.reason == 1          ? "Self-check skipped"
                                                    : "Self-check not run: no reference in data";
    boot_screen(verdict, 1, 1, result == SELFTEST_PASS ? C_BAR : result == SELFTEST_FAIL ? C_BAD : C_TITLE);
    for (int k = 0; k < 8; k++) plat_sleep(200);

    wait_no_key_pressed();
    plat_input_t in;
    plat_poll(&in); /* prime edge detection */
    uint32_t frames = 0;
    for (;;) {
        plat_poll(&in);
        if (!app_frame(&app, &in)) break;
        frames++;
        if (frames == 50u || frames == 500u)
            logf_line("frames=%lu step=%lu speed_permille=%lu", frames, sim.step, app.sim_permille);
    }
    logf_line("exit frames=%lu spikes=%lu eps=%lu", frames, (unsigned long)sim.total_spikes, app.edges_per_sec);
    logf_line("exit clock_ms=%lu rtc_s=%lu render_ms=%lu", plat_ms() - boot_ms, plat_rtc() - boot_rtc, app.render_ms / 16u);
    logf_line("exit faults=%lu flushes=%lu step=%lu", sim.page_faults, sim.pool_flushes, sim.step);
    app_free(&app);
close_sim:
    fs_close(&sim);
out:
    plat_timer_restore();
    lcd_init(SCR_TYPE_INVALID);
    return 0;
}
