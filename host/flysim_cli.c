/*
 * flysim_cli — run the calculator's simulation core on the host.
 *
 *   flysim_cli <data-dir> [--arena-mb N | --arena-chunks N] [--scenario K] [--side both|L|R]
 *              [--rate HZ] [--gain UV] [--steps N] [--no-weak]
 *              [--row TIER NEURON] [--bench] [--silence-every K]
 *              [--at STEP ACTION]... [--bench-first] [--fake-clock]
 *
 * --at changes a setting just before step STEP (in the order given):
 *   gain=UV | rate=HZ | silence=K | unsilence=K   (K: every K-th neuron)
 * --bench-first makes the calls of the on-calculator benchmark before the
 * run, to show that they are harmless.
 * --fake-clock gives the core a profiling clock that advances by one each
 * time it is read.
 *
 * Prints one JSON object so tests can compare it with the Python mirror.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "flysim.h"

static fs_sim_t sim;

static void progress(void *ctx, uint32_t done, uint32_t total) {
    (void)ctx;
    (void)done;
    (void)total;
}

static uint32_t fake_now;
static uint32_t fake_clock(void) { return fake_now++; }

enum { MAX_EVENTS = 64 };
typedef struct {
    uint32_t step;
    const char *action;
} event_t;

static void silence_every(uint32_t k, int on) {
    if (k == 0) return;
    for (uint32_t i = 0; i < sim.n; i += k) fs_set_silenced(&sim, i, on);
}

/* Returns 0 if the action is not understood. */
static int apply_action(const char *action, const uint32_t *stim_ids, uint32_t n_stim) {
    const char *eq = strchr(action, '=');
    if (!eq) return 0;
    size_t len = (size_t)(eq - action);
    char *end = NULL;
    uint32_t value = (uint32_t)strtoul(eq + 1, &end, 10);
    if (end == eq + 1 || *end != '\0') return 0; /* not a number */
    if (len == 4 && !memcmp(action, "gain", 4)) fs_set_gain(&sim, value);
    else if (len == 4 && !memcmp(action, "rate", 4)) fs_set_stim(&sim, 0, stim_ids, n_stim, value);
    else if (len == 7 && !memcmp(action, "silence", 7)) silence_every(value, 1);
    else if (len == 9 && !memcmp(action, "unsilence", 9)) silence_every(value, 0);
    else return 0;
    return 1;
}

static int dump_row(unsigned tier, uint32_t neuron) {
    enum { CAP = 1 << 16 };
    uint32_t *t = malloc(CAP * sizeof *t), *w = malloc(CAP * sizeof *w);
    if (!t || !w) {
        free(t);
        free(w);
        return 1;
    }
    int32_t c = fs_decode_row(&sim, tier, neuron, t, w, CAP);
    if (c < 0) {
        fprintf(stderr, "decode failed: %s\n", fs_strerror(c));
        return 1;
    }
    printf("{\"tier\": %u, \"neuron\": %u, \"edges\": [", tier, neuron);
    for (int32_t e = 0; e < c; e++) printf("%s[%u, %u]", e ? ", " : "", t[e], w[e]);
    printf("]}\n");
    free(t);
    free(w);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: flysim_cli <data-dir> [options]\n");
        return 2;
    }
    const char *dir = argv[1];
    uint32_t arena_bytes = 0, steps = 1000, gain = 0, rate = 0;
    int scenario = -1, side = 0, no_weak = 0, bench = 0, bench_first = 0, fake = 0, row_tier = -1;
    uint32_t bench_edges = 0;
    uint64_t bench_replayed = 0;
    uint32_t row_neuron = 0, silence_k = 0, n_events = 0;
    event_t events[MAX_EVENTS];
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : "";
        if (!strcmp(a, "--arena-mb")) { arena_bytes = (uint32_t)strtoul(v, NULL, 10) << 20; i++; }
        else if (!strcmp(a, "--arena-chunks")) { arena_bytes = (uint32_t)strtoul(v, NULL, 10) * FS_CHUNK_BYTES; i++; }
        else if (!strcmp(a, "--scenario")) { scenario = atoi(v); i++; }
        else if (!strcmp(a, "--steps")) { steps = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--gain")) { gain = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--rate")) { rate = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--side")) { side = (v[0] == 'L') ? 1 : (v[0] == 'R') ? 2 : 0; i++; }
        else if (!strcmp(a, "--silence-every")) { silence_k = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--at")) {
            if (i + 2 >= argc || n_events >= MAX_EVENTS) {
                fprintf(stderr, "--at needs STEP ACTION (at most %d of them)\n", (int)MAX_EVENTS);
                return 2;
            }
            events[n_events].step = (uint32_t)strtoul(argv[i + 1], NULL, 10);
            events[n_events++].action = argv[i + 2];
            i += 2;
        }
        else if (!strcmp(a, "--no-weak")) no_weak = 1;
        else if (!strcmp(a, "--bench")) bench = 1;
        else if (!strcmp(a, "--bench-first")) bench_first = 1;
        else if (!strcmp(a, "--fake-clock")) fake = 1;
        else if (!strcmp(a, "--row") && i + 2 < argc) { row_tier = atoi(argv[i + 1]); row_neuron = (uint32_t)strtoul(argv[i + 2], NULL, 10); i += 2; }
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    int rc = fs_open(&sim, dir, "flymeta.tns", arena_bytes, 0, progress, NULL);
    if (rc != FS_OK) {
        fprintf(stderr, "fs_open: %s\n", fs_strerror(rc));
        return 1;
    }
    if (row_tier >= 0) {
        int r = dump_row((unsigned)row_tier, row_neuron);
        fs_close(&sim);
        return r;
    }
    if (bench_first) { /* the sequence of calc/bench.c */
        bench_edges = fs_bench_rows(&sim, 0, 6000, 0) + fs_bench_rows(&sim, 0, 6000, 1);
        fs_reset(&sim);
        fs_bench_wake_all(&sim, 64);
        fs_step(&sim);
        (void)fs_bench_catch_up(&sim, 4096, 0);
        bench_replayed = fs_bench_catch_up(&sim, 4096, 24);
        fs_reset(&sim);
    }
    if (gain) fs_set_gain(&sim, gain);
    if (no_weak && sim.hdr->n_tiers > 1) sim.tier[1].enabled = 0;
    silence_every(silence_k, 1);
    uint32_t n_stim = 0;
    const uint32_t *stim_ids = NULL;
    if (scenario >= 0) {
        uint32_t size = 0;
        const fs_scenario_t *sc = fs_section(&sim, "SCEN", &size);
        if (!sc || (uint32_t)scenario >= sim.hdr->n_scenarios) {
            fprintf(stderr, "no such scenario\n");
            fs_close(&sim);
            return 1;
        }
        sc += scenario;
        const uint32_t *ids = (const uint32_t *)(const void *)((const uint8_t *)fs_section(&sim, "SCEN", NULL) + sc->ids_off);
        uint32_t n = sc->n_ids;
        if (side == 1) n = sc->n_left;
        if (side == 2) { ids += sc->n_left; n = sc->n_right; }
        fs_set_stim(&sim, 0, ids, n, rate ? rate : sc->rate_hz);
        n_stim = n;
        stim_ids = ids;
    }
    if (fake) sim.ticks = fake_clock;
    clock_t t0 = clock();
    for (uint32_t k = 0; k < steps; k++) {
        for (uint32_t e = 0; e < n_events; e++) {
            if (events[e].step != k) continue;
            if (!apply_action(events[e].action, stim_ids, n_stim)) {
                fprintf(stderr, "unknown action %s\n", events[e].action);
                fs_close(&sim);
                return 2;
            }
        }
        fs_step(&sim);
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("{\"neurons\": %u, \"steps\": %u, \"stimulated\": %u, \"spikes\": %llu, \"edges\": %llu, \"hash\": %u, "
           "\"theta\": %d, \"io_errors\": %u, \"chunks\": %u, \"pool_first\": %u, \"resident_bytes\": %u, \"total_bytes\": %u, "
           "\"page_faults\": %u, \"promotions\": %u, \"pool_flushes\": %u, "
           "\"awake\": %u, \"awake_updates\": %llu, \"catchup_steps\": %llu, \"bound_shift\": %u, "
           "\"ticks_deliver\": %llu, \"ticks_integrate\": %llu, \"ticks_fault\": %llu",
           sim.n, sim.step, n_stim, (unsigned long long)sim.total_spikes, (unsigned long long)sim.total_edges,
           sim.hash, sim.theta, sim.io_errors, sim.n_chunks, sim.pool_first, sim.resident_bytes, sim.total_bytes,
           sim.page_faults, sim.promotions, sim.pool_flushes, sim.n_awake,
           (unsigned long long)sim.awake_updates, (unsigned long long)sim.catchup_steps, sim.bound_shift,
           (unsigned long long)sim.ticks_deliver, (unsigned long long)sim.ticks_integrate,
           (unsigned long long)sim.ticks_fault);
    if (bench) printf(", \"seconds\": %.4f, \"edges_per_sec\": %.0f", secs, secs > 0 ? (double)sim.total_edges / secs : 0.0);
    if (bench_first) printf(", \"bench_edges\": %u, \"bench_replayed\": %llu", bench_edges, (unsigned long long)bench_replayed);
    printf(", \"group_counts\": [");
    for (uint32_t g = 0; g < sim.hdr->n_groups; g++) printf("%s%u", g ? ", " : "", sim.group_count[g]);
    printf("]}\n");
    fs_close(&sim);
    return 0;
}
