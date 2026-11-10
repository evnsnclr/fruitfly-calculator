/*
 * Optional on-device measurements, written to the log.  Runs at boot only if
 * a file named flybench.tns sits next to the program.  It answers the
 * questions that cannot be answered from the host: how the OS maps and
 * caches our memory, what a memory access costs, and where simulation and
 * drawing time go.
 */
#include <libndls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

#define BENCH_ITERS 400000u
#define G_WORDS 0x10000u /* 65536 int32 = 256 KB: far larger than the 8 KB data cache */
#define G_MASK (G_WORDS - 1u)
#define SMALL_BYTES 2048u /* a table that fits in the data cache */

static void logf4(const char *fmt, unsigned long a, unsigned long b, unsigned long c, unsigned long d) {
    char line[200];
    snprintf(line, sizeof line, fmt, a, b, c, d);
    plat_log(line);
}

static uint32_t cp15_cache_type(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(v));
    return v;
}

static uint32_t cp15_control(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

static uint32_t cp15_ttbr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v));
    return v;
}

#define RAM_BASE 0x10000000u
#define RAM_END 0x14000000u

/* First- and (for page tables) second-level MMU descriptors of an address.
 * The tables are read through the identity mapping of RAM, as Ndless itself
 * does; anything that does not look like RAM is not followed. */
static void describe_mapping(const char *what, const void *addr) {
    uint32_t ttbr = cp15_ttbr() & ~0x3FFFu, a = (uint32_t)addr, d1 = 0, d2 = 0;
    if (ttbr >= RAM_BASE && ttbr < RAM_END) {
        const uint32_t *l1 = (const uint32_t *)ttbr;
        d1 = l1[a >> 20];
        uint32_t l2 = d1 & ~0x3FFu;
        if ((d1 & 3u) == 1u && l2 >= RAM_BASE && l2 < RAM_END && (l1[l2 >> 20] & 3u) == 2u)
            d2 = ((const uint32_t *)l2)[(a >> 12) & 0xFFu];
    }
    char line[120];
    snprintf(line, sizeof line, "map %s addr=%08lx l1=%08lx l2=%08lx", what, (unsigned long)a, (unsigned long)d1,
             (unsigned long)d2);
    plat_log(line);
}

static inline uint32_t lcg(uint32_t x) { return x * 1664525u + 1013904223u; }

/* Nanoseconds per iteration of a loop that took `ms` for BENCH_ITERS. */
static unsigned long ns_each(uint32_t ms) { return (unsigned long)(((uint64_t)ms * 1000000u) / BENCH_ITERS); }

static void memory_costs(void) {
    volatile uint32_t sink = 0;
    uint32_t x = 12345u, acc = 0, t0;
    int32_t *g = calloc(G_WORDS, sizeof(int32_t));
    uint8_t *small = calloc(SMALL_BYTES, 1);
    if (!g || !small) {
        plat_log("mem: no spare memory for the measurement");
        free(g);
        free(small);
        return;
    }

    t0 = plat_ms();
    for (uint32_t i = 0; i < BENCH_ITERS; i++) {
        x = lcg(x);
        acc += x >> 15;
    }
    uint32_t base = plat_ms() - t0;

    t0 = plat_ms();
    for (uint32_t i = 0; i < BENCH_ITERS; i++) {
        x = lcg(x);
        acc += (uint32_t)g[(x >> 15) & G_MASK];
    }
    uint32_t rand_read = plat_ms() - t0;

    t0 = plat_ms();
    for (uint32_t i = 0; i < BENCH_ITERS; i++) {
        x = lcg(x);
        g[(x >> 15) & G_MASK] += 1;
    }
    uint32_t rand_rmw = plat_ms() - t0;

    t0 = plat_ms();
    for (uint32_t i = 0; i < BENCH_ITERS; i++) {
        x = lcg(x);
        small[(x >> 15) & 2047u] = 1;
    }
    uint32_t small_store = plat_ms() - t0;

    t0 = plat_ms();
    for (uint32_t i = 0; i < BENCH_ITERS; i++) {
        x = lcg(x);
        uint8_t *p = &small[(x >> 15) & 2047u];
        if (!*p) *p = 1;
    }
    uint32_t small_test = plat_ms() - t0;

    /* interleave: a random 512 KB access between each small-table access,
     * the pattern of the delivery loop */
    t0 = plat_ms();
    for (uint32_t i = 0; i < BENCH_ITERS; i++) {
        x = lcg(x);
        g[(x >> 15) & G_MASK] += 1;
        small[(x >> 9) & 2047u] = 1;
    }
    uint32_t mixed_store = plat_ms() - t0;

    t0 = plat_ms();
    for (uint32_t i = 0; i < BENCH_ITERS; i++) {
        x = lcg(x);
        g[(x >> 15) & G_MASK] += 1;
        uint8_t *p = &small[(x >> 9) & 2047u];
        if (!*p) *p = 1;
    }
    uint32_t mixed_test = plat_ms() - t0;

    t0 = plat_ms();
    for (uint32_t pass = 0; pass < 4u; pass++)
        for (uint32_t i = 0; i < G_WORDS; i++) acc += (uint32_t)g[i];
    uint32_t seq_ms = plat_ms() - t0; /* 4 passes over 256 KB */

    sink = acc;
    (void)sink;
    free(g);
    free(small);
    logf4("mem ns: loop=%lu rand_read=%lu rand_rmw=%lu seq_read_4x256k_ms=%lu", ns_each(base), ns_each(rand_read),
          ns_each(rand_rmw), seq_ms);
    logf4("mem ns: small_store=%lu small_test=%lu mixed_store=%lu mixed_test=%lu", ns_each(small_store), ns_each(small_test),
          ns_each(mixed_store), ns_each(mixed_test));
}

static void simulation_costs(fs_sim_t *s) {
    uint32_t t0 = plat_ms();
    uint32_t e_decode = fs_bench_rows(s, 0, 6000, 0);
    uint32_t ms_decode = plat_ms() - t0;
    t0 = plat_ms();
    uint32_t e_apply = fs_bench_rows(s, 0, 6000, 1);
    uint32_t ms_apply = plat_ms() - t0;
    logf4("rows: decode edges=%lu ms=%lu | apply edges=%lu ms=%lu", e_decode, ms_decode, e_apply, ms_apply);

    /* Every neuron awake with a little drive, nothing spiking: the cost of
     * the per-tick pass per neuron.  They all go back to sleep after it. */
    fs_reset(s);
    fs_clear_stims(s);
    fs_bench_wake_all(s, 64);
    t0 = plat_ms();
    fs_step(s);
    uint32_t all_awake = plat_ms() - t0;
    /* The cost of replaying missed steps: the same sweep with and without
     * 24 steps to catch up. */
    t0 = plat_ms();
    (void)fs_bench_catch_up(s, 4096, 0);
    uint32_t sweep = plat_ms() - t0;
    t0 = plat_ms();
    uint64_t replayed = fs_bench_catch_up(s, 4096, 24);
    uint32_t catch_up = plat_ms() - t0;
    logf4("step: all_awake_ms=%lu sweep_ms=%lu catch_up_ms=%lu replayed=%lu", all_awake, sweep, catch_up,
          (unsigned long)replayed);
    fs_reset(s);
    /* and of an idle step */
    t0 = plat_ms();
    for (int k = 0; k < 200; k++) fs_step(s);
    uint32_t idle200 = plat_ms() - t0;
    logf4("step: idle_200_steps_ms=%lu spikes=%lu", idle200, (unsigned long)s->total_spikes, 0, 0);
    fs_reset(s);
}

static void drawing_costs(app_t *a) {
    uint32_t t0 = plat_ms();
    for (int k = 0; k < 10; k++) plat_present(a->gfx.px);
    uint32_t present10 = plat_ms() - t0;
    t0 = plat_ms();
    for (int k = 0; k < 10; k++) app_render(a);
    uint32_t render10 = plat_ms() - t0;
    logf4("draw: present_10_ms=%lu render_10_ms=%lu", present10, render10, 0, 0);
}

int bench_requested(void) {
    FILE *f = fopen("flybench.tns", "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

void bench_run(fs_sim_t *s, app_t *a) {
    static uint32_t on_stack_marker;
    logf4("cp15 cache_type=%08lx control=%08lx ttbr=%08lx", cp15_cache_type(), cp15_control(), cp15_ttbr(), 0);
    describe_mapping("cells", s->cell);
    describe_mapping("arena", s->chunk[0]);
    describe_mapping("meta", s->meta);
    describe_mapping("code", (const void *)(uintptr_t)&bench_run);
    describe_mapping("bss", &on_stack_marker);
    describe_mapping("lcd", REAL_SCREEN_BASE_ADDRESS);
    memory_costs();
    fs_reset(s);
    simulation_costs(s);
    drawing_costs(a);
    app_select_scenario(a, 0);
}
