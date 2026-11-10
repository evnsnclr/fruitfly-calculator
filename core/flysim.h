/*
 * flysim — whole-connectome spiking simulator for small machines.
 *
 * Portable C99, integer only.  The same source is compiled for the
 * TI-Nspire CX II (ARM926EJ-S, no FPU) and for the host, where it is checked
 * bit-for-bit against the Python mirror in pipeline/intsim.py.
 *
 * Model: leaky integrate-and-fire with an exponentially decaying synaptic
 * variable, the equations and reset rule of Shiu et al. 2024 (v and g are
 * frozen while refractory, a spike resets both), integrated exactly over a
 * fixed step.
 * Connectivity: every neuron-to-neuron connection of the Janelia male CNS
 * connectome v1.0, entropy coded on storage and decoded on the fly each
 * time a neuron spikes.
 */
#ifndef FLYSIM_H
#define FLYSIM_H

#include <stdint.h>
#include <stdio.h>

#define FS_MAGIC "FLYCNS01"
#define FS_VERSION 5u

#define FS_BLOCK_SHIFT 6
#define FS_BLOCK_ROWS (1u << FS_BLOCK_SHIFT) /* presynaptic neurons per block */
#define FS_MAX_TIERS 2
#define FS_MAX_STIM 8
#define FS_SPIKE_CAP 8192u /* per-step spike list exposed to the UI */
#define FS_FRAC 8          /* fixed-point fractional bits of v and g */
#define FS_GAP_LUT_BITS 10
#define FS_W_LUT_BITS 9
#define FS_GAP_EXACT 16u /* gaps 1..16 have their own symbol */
#define FS_W_EXACT 8u    /* weight offsets 0..7 have their own symbol */
/* The arena is allocated in chunks this size: small enough to use a
 * fragmented heap, large enough for the biggest block (about 65 KB). */
#define FS_CHUNK_BYTES (1u << 18)
#define FS_MAX_CHUNKS 192
#define FS_MIN_POOL_CHUNKS 8u /* 2 MB of paging pool unless the arena is tiny */
#define FS_BLOCK_SLACK 16u /* readable bytes past a block (bit reader) */
#define FS_MAX_BLOCK_BYTES (FS_CHUNK_BYTES - FS_BLOCK_SLACK - 4u)
#define FS_GAP_SYMS 30u /* gap symbols: 16 exact + log2 buckets up to 2^18 */
#define FS_W_SYMS 20u   /* weight symbols: 8 exact + log2 buckets up to 2^15 */
#define FS_V_FORCE 0x40000000 /* membrane value that guarantees a spike */

/* Where a block of edges currently lives. */
#define FS_AT_FLASH 0u  /* not in RAM: read on demand */
#define FS_AT_PINNED 1u /* preloaded, never evicted */
#define FS_AT_SPACE0 2u /* paging pool, first half */
#define FS_AT_SPACE1 3u /* paging pool, second half */

/* Neuron flag bits (meta section NFLG). */
#define FS_SIGN_MASK 3u
#define FS_SIGN_EXC 0u
#define FS_SIGN_INH 1u
#define FS_SIGN_NONE 2u /* neuromodulatory: no fast synaptic effect */
#define FS_SIDE_SHIFT 2
#define FS_SIDE_MASK 3u /* after shift: 0 unsided, 1 left, 2 right */

enum {
    FS_OK = 0,
    FS_ERR_IO = -1,
    FS_ERR_FORMAT = -2,
    FS_ERR_MEMORY = -3,
    FS_ERR_ARG = -4
};

/* ---- on-disk structures (little endian, all fields 4-byte aligned) ---- */

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t n_neurons;
    uint32_t n_types;
    uint32_t n_groups;
    uint32_t n_scenarios;
    uint32_t n_blocks;
    uint32_t n_tiers;
    uint32_t n_edges[FS_MAX_TIERS];
    uint32_t n_synapses[FS_MAX_TIERS];
    int32_t dv, dg, cv;   /* Q16 step multipliers */
    uint32_t v_thresh_uv; /* spike threshold above rest, microvolts */
    uint32_t reserved0;
    uint32_t w_syn_uv;    /* default gain: microvolts of g per synapse */
    uint32_t delay, refrac;
    uint32_t dt_us;
    uint32_t seed;
    uint32_t map_w, map_h;
    uint32_t n_sections;
} fs_header_t;

typedef struct {
    char tag[4];
    uint32_t offset;
    uint32_t size;
} fs_section_t;

typedef struct {
    char name[20];
    char hint[28];
    uint32_t rate_hz;
    uint32_t n_ids, n_left, n_right; /* ids are ordered left, right, unsided */
    uint32_t ids_off;                /* byte offset of uint32 ids from section start */
} fs_scenario_t;

typedef struct {
    char name[12];
    uint16_t rgb565;
    uint16_t flags; /* bit0: motor pool */
    uint32_t size;  /* neurons in the group */
} fs_group_t;

typedef struct {
    char file[32];
    uint32_t wmin;        /* weight stored as (w - wmin) */
    uint32_t has_weights; /* 0: every edge has weight wmin */
    uint32_t file_bytes;
    uint32_t max_block_bytes;
    uint16_t gap_lut[1u << FS_GAP_LUT_BITS];
    uint16_t w_lut[1u << FS_W_LUT_BITS];
    /* followed by uint32_t block_off[n_blocks + 1] */
} fs_tier_desc_t;

/* ---- runtime ---- */

/* One neuron.  Sixteen bytes, aligned, so that touching a neuron costs one
 * cache line: on the calculator a line fill takes as long as ~150
 * instructions, and that, not arithmetic, is what a step is made of.
 *
 * Most neurons with non-zero state cannot reach threshold unless more input
 * arrives.  Those are "asleep": nothing is done for them per step.  `stamp`
 * records the step their state belongs to, and whoever touches them next
 * (an arriving spike, a stimulus, a parameter change) first replays the
 * missed steps.  The result is identical, step for step, to integrating
 * every neuron every time.  (The step counter is 32 bits: a neuron left
 * untouched with non-zero state for exactly 2^32 steps, 49 days of
 * simulated time, would be taken for current.) */
typedef struct {
    int32_t v, g;   /* membrane potential and synaptic drive, FS_FRAC fixed point */
    uint32_t stamp; /* v and g are the state at the start of this step */
    uint8_t tmr;    /* spike timer (delay and refractoriness); non-zero only while awake */
    uint8_t flags;  /* FS_CELL_* */
    uint16_t reserved;
} fs_cell_t;

#define FS_CELL_SILENCED 1u /* never spikes; its input still decays */
#define FS_CELL_AWAKE 2u    /* mirrors the bit in fs_sim_t.awake */
#define FS_NO_BOUND 255u

typedef struct {
    const uint32_t *ids;
    uint32_t n;
    uint32_t thresh; /* P(event per step) * 2^32 */
} fs_stim_t;

typedef struct {
    const fs_tier_desc_t *desc;
    const uint32_t *block_off;
    FILE *fp;
    uint8_t **block; /* [n_blocks] resident block or NULL */
    uint8_t *where;  /* [n_blocks] FS_AT_* */
    int enabled;
} fs_tier_t;

typedef void (*fs_progress_fn)(void *ctx, uint32_t done_bytes, uint32_t total_bytes);

typedef struct {
    /* meta (owned buffer + views into it) */
    uint8_t *meta;
    uint32_t meta_bytes;
    const fs_header_t *hdr;
    const uint8_t *flags;   /* NFLG [n] */
    const uint16_t *pix;    /* NPIX [n] */
    const uint16_t *type;   /* NTYP [n] */
    const uint8_t *group;   /* NGRP [n] */

    uint32_t n, n_blocks;

    /* state */
    fs_cell_t *cell;    /* [n], cache-line aligned */
    void *cell_mem;     /* the allocation `cell` points into */
    uint32_t *awake;     /* [n_words] bitmap of the neurons stepped every tick */
    uint32_t *awake_sum; /* [n_sum] bitmap of the non-zero words of `awake` */
    uint32_t n_words, n_sum;
    uint32_t n_awake;
    uint32_t bound_shift; /* see may_fire(); FS_NO_BOUND if the constants allow none */
    int32_t v_linear;     /* |v| at or below this decays by exactly 1 per step */

    /* parameters (mutable gain) */
    int32_t dv, dg, cv, theta;
    uint32_t w_syn_uv;
    uint8_t timer_start, timer_deliver, timer_refrac_min;

    /* connectivity */
    fs_tier_t tier[FS_MAX_TIERS];
    uint8_t *chunk[FS_MAX_CHUNKS];
    uint32_t n_chunks, pool_first; /* chunks >= pool_first form the paging pool */
    uint32_t fill_chunk, fill_off; /* preload bump pointer */
    /* The pool is two half-spaces.  New and recently used blocks live in the
     * active one; when it fills, the other is recycled.  A block touched in
     * the old half is copied across, so only blocks unused for a whole
     * generation are dropped (a cheap approximation of LRU). */
    uint32_t space_first[2], space_end[2];
    uint32_t active;               /* 0 or 1 */
    uint32_t pool_chunk, pool_off; /* bump pointer inside the active half */
    uint32_t resident_bytes, total_bytes;
    const uint16_t *hot; /* optional ranking of (tier << 15 | block) units, most used first */

    /* stimulus */
    fs_stim_t stim[FS_MAX_STIM];
    uint32_t rng;

    /* monitors */
    uint32_t step;
    uint64_t total_spikes;
    uint64_t total_edges; /* synaptic events delivered */
    uint32_t hash;        /* FNV-1a over (neuron, step) of every spike */
    uint32_t n_spikes;    /* spikes in the last step */
    uint32_t spikes[FS_SPIKE_CAP];
    uint32_t n_listed;    /* min(n_spikes, FS_SPIKE_CAP) */
    uint32_t *type_count; /* [n_types] spikes since last clear */
    uint32_t *group_count; /* [n_groups] */
    uint32_t page_faults;  /* blocks read from flash while running */
    uint32_t promotions;   /* blocks kept by copying into the active half */
    uint32_t pool_flushes; /* generations */
    uint32_t io_errors; /* rows that could not be read or decoded (edges lost) */
    /* Optional profiling.  If `ticks` is set (any free-running counter),
     * fs_step adds up how long delivery takes, how much of that is reading
     * blocks from storage, and how long the rest of the step takes
     * (stimulus and integration). */
    uint32_t (*ticks)(void);
    uint64_t ticks_deliver, ticks_integrate, ticks_fault;
    uint64_t awake_updates; /* neuron steps integrated in the per-tick pass */
    uint64_t catchup_steps; /* loop iterations spent replaying steps for sleeping neurons */
    int arena_exhausted; /* malloc ran dry while sizing the arena */
    uint32_t bench_sink;
} fs_sim_t;

/* Open <dir>/<meta_name>, allocate state, and load as much connectivity as
 * fits in at most max_arena_bytes of RAM (0 = as much as malloc will give,
 * minus reserve_bytes handed back for the OS).  Blocks that do not fit are
 * demand-paged from storage while running. */
int fs_open(fs_sim_t *s, const char *dir, const char *meta_name, uint32_t max_arena_bytes,
            uint32_t reserve_bytes, fs_progress_fn progress, void *ctx);
void fs_close(fs_sim_t *s);

void fs_reset(fs_sim_t *s);           /* zero all neural state, keep stimuli */
void fs_step(fs_sim_t *s);            /* advance one time step */
void fs_set_gain(fs_sim_t *s, uint32_t w_syn_uv);
void fs_set_silenced(fs_sim_t *s, uint32_t neuron, int silenced);
int fs_is_silenced(const fs_sim_t *s, uint32_t neuron);
void fs_set_stim(fs_sim_t *s, unsigned slot, const uint32_t *ids, uint32_t n, uint32_t rate_hz);
void fs_clear_stims(fs_sim_t *s);
void fs_clear_counts(fs_sim_t *s);

const void *fs_section(const fs_sim_t *s, const char *tag, uint32_t *size);
const char *fs_strerror(int code);

/* Speed measurement on the target: see flysim.c.  All of these disturb the
 * neural state; call fs_reset() afterwards. */
uint32_t fs_bench_rows(fs_sim_t *s, uint32_t first, uint32_t count, int apply);
void fs_bench_wake_all(fs_sim_t *s, int32_t g);
uint64_t fs_bench_catch_up(fs_sim_t *s, int32_t g, uint32_t gap);

/* Decode one neuron's outgoing connections in one tier (testing/inspection).
 * Returns the number of edges, or a negative error. */
int32_t fs_decode_row(fs_sim_t *s, unsigned tier, uint32_t neuron, uint32_t *targets, uint32_t *weights,
                      uint32_t cap);

#endif
