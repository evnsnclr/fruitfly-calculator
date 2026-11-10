/* flysim core.  See flysim.h for the model and file-format overview. */
#include "flysim.h"

#include <stdlib.h>
#include <string.h>

#define CELL_ALIGN 32u      /* one ARM926 cache line: two cells, none straddling */
#define MAX_BOUND_SHIFT 8u
typedef char fs_cell_is_half_a_cache_line[(sizeof(fs_cell_t) * 2u == CELL_ALIGN) ? 1 : -1];

/* ------------------------------------------------------------------ util */

const char *fs_strerror(int code) {
    switch (code) {
    case FS_OK: return "ok";
    case FS_ERR_IO: return "file missing or unreadable";
    case FS_ERR_FORMAT: return "bad or mismatched data file";
    case FS_ERR_MEMORY: return "not enough memory";
    case FS_ERR_ARG: return "bad argument";
    default: return "unknown error";
    }
}

static FILE *open_in_dir(const char *dir, const char *name) {
    char path[256];
    int n = snprintf(path, sizeof path, "%s%s%s", dir ? dir : "", (dir && dir[0]) ? "/" : "", name);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    return fopen(path, "rb");
}

const void *fs_section(const fs_sim_t *s, const char *tag, uint32_t *size) {
    const fs_section_t *sec = (const fs_section_t *)(s->meta + sizeof(fs_header_t));
    for (uint32_t i = 0; i < s->hdr->n_sections; i++) {
        if (memcmp(sec[i].tag, tag, 4) == 0) {
            if (size) *size = sec[i].size;
            return s->meta + sec[i].offset;
        }
    }
    if (size) *size = 0;
    return NULL;
}

/* Section lookup that also checks the minimum size the caller will index. */
static const void *need_section(const fs_sim_t *s, const char *tag, uint32_t min_size) {
    uint32_t size = 0;
    const void *p = fs_section(s, tag, &size);
    return (p && size >= min_size) ? p : NULL;
}

/* ----------------------------------------------------------------- arena */

/* Bump cursor over the arena chunks; a block never straddles two chunks. */
static int cursor_take(uint32_t *chunk, uint32_t *off, uint32_t end_chunk, uint32_t bytes, uint32_t *at_chunk,
                       uint32_t *at_off) {
    if (bytes > FS_MAX_BLOCK_BYTES || *chunk >= end_chunk) return 0;
    uint32_t need = (bytes + FS_BLOCK_SLACK + 3u) & ~3u;
    if (*off + need > FS_CHUNK_BYTES) {
        /* In the last chunk a block that does not fit is refused without
         * moving on, so a smaller one can still use the tail. */
        if (*chunk + 1u >= end_chunk) return 0;
        (*chunk)++;
        *off = 0;
    }
    *at_chunk = *chunk;
    *at_off = *off;
    *off += need;
    return 1;
}

static uint8_t *bump(fs_sim_t *s, uint32_t *chunk, uint32_t *off, uint32_t end_chunk, uint32_t bytes) {
    uint32_t c, o;
    return cursor_take(chunk, off, end_chunk, bytes, &c, &o) ? s->chunk[c] + o : NULL;
}

/* Would every block of every tier fit in n_chunks chunks? */
static int everything_fits(const fs_sim_t *s, uint32_t n_chunks) {
    uint32_t chunk = 0, off = 0, c, o;
    for (unsigned k = 0; k < s->hdr->n_tiers; k++) {
        const uint32_t *bo = s->tier[k].block_off;
        for (uint32_t b = 0; b < s->n_blocks; b++)
            if (!cursor_take(&chunk, &off, n_chunks, bo[b + 1] - bo[b], &c, &o)) return 0;
    }
    return 1;
}

/* The 64 row lengths at the head of a block must describe rows that lie
 * inside it; everything the decoder does later relies on this. */
static int block_ok(const uint8_t *blk, uint32_t size) {
    const uint16_t *len = (const uint16_t *)(const void *)blk;
    uint32_t sum = FS_BLOCK_ROWS * 2u;
    if (size < sum) return 0;
    for (uint32_t k = 0; k < FS_BLOCK_ROWS; k++) sum += len[k];
    return sum <= size;
}

static int read_block(fs_tier_t *t, uint32_t b, uint8_t *dst) {
    uint32_t off = t->block_off[b], size = t->block_off[b + 1] - off;
    if (fseek(t->fp, (long)off, SEEK_SET) != 0) return FS_ERR_IO;
    if (size && fread(dst, 1, size, t->fp) != size) return FS_ERR_IO;
    memset(dst + size, 0, FS_BLOCK_SLACK);
    return block_ok(dst, size) ? FS_OK : FS_ERR_FORMAT;
}

/* Start a new generation: recycle the half that was not active.  Whatever
 * still lives there was not used during the last generation and is dropped;
 * the half we are leaving becomes the "old" one, from which blocks are
 * rescued by copying when they are needed again. */
static void pool_flip(fs_sim_t *s) {
    uint32_t next = (s->space_end[1] > s->space_first[1]) ? 1u - s->active : s->active;
    uint8_t stale = (uint8_t)(FS_AT_SPACE0 + next);
    for (unsigned k = 0; k < s->hdr->n_tiers; k++) {
        fs_tier_t *t = &s->tier[k];
        for (uint32_t b = 0; b < s->n_blocks; b++) {
            if (t->where[b] == stale) {
                t->where[b] = FS_AT_FLASH;
                t->block[b] = NULL;
            }
        }
    }
    s->active = next;
    s->pool_chunk = s->space_first[next];
    s->pool_off = 0;
    s->pool_flushes++;
}

/* Cold path of block_get: bring a block into the active half of the pool,
 * from the old half if it is still there, otherwise from storage.  Returns
 * NULL on I/O failure or if there is no pool to put it in. */
static const uint8_t *block_fault(fs_sim_t *s, fs_tier_t *t, uint32_t b) {
    uint32_t size = t->block_off[b + 1] - t->block_off[b];
    uint32_t mark_chunk = s->pool_chunk, mark_off = s->pool_off;
    uint8_t *dst = bump(s, &s->pool_chunk, &s->pool_off, s->space_end[s->active], size);
    if (!dst) {
        if (s->space_end[0] == s->space_first[0]) return NULL; /* no pool at all */
        pool_flip(s);
        mark_chunk = s->pool_chunk;
        mark_off = s->pool_off;
        dst = bump(s, &s->pool_chunk, &s->pool_off, s->space_end[s->active], size);
        if (!dst) return NULL;
    }
    /* A flip may just have dropped this very block: look again. */
    if (t->where[b] != FS_AT_FLASH) {
        memcpy(dst, t->block[b], size + FS_BLOCK_SLACK);
        s->promotions++;
    } else {
        const uint32_t t0 = s->ticks ? s->ticks() : 0u;
        int rc = read_block(t, b, dst);
        if (s->ticks) s->ticks_fault += (uint32_t)(s->ticks() - t0);
        if (rc != FS_OK) {
            /* Give the space back, or a block that keeps failing would eat
             * the pool and evict healthy blocks. */
            s->pool_chunk = mark_chunk;
            s->pool_off = mark_off;
            return NULL;
        }
        s->page_faults++;
    }
    t->block[b] = dst;
    t->where[b] = (uint8_t)(FS_AT_SPACE0 + s->active);
    return dst;
}

/* Resident pointer for a block of edges.  The result is only guaranteed to
 * stay valid until the next call. */
static inline const uint8_t *block_get(fs_sim_t *s, fs_tier_t *t, uint32_t b) {
    uint32_t w = t->where[b];
    if (w == FS_AT_PINNED || w == FS_AT_SPACE0 + s->active) return t->block[b];
    return block_fault(s, t, b);
}

/* --------------------------------------------------------------- decoder */

typedef struct {
    const uint8_t *p;
    uint32_t buf; /* bits are consumed from the top */
    uint32_t cnt; /* valid bits in buf */
} bits_t;

#define REFILL(b)                                              \
    while ((b).cnt <= 24u) {                                   \
        (b).buf |= (uint32_t)(*(b).p++) << (24u - (b).cnt);    \
        (b).cnt += 8u;                                         \
    }
#define DROP(b, n) ((b).buf <<= (n), (b).cnt -= (n))

/* Locate row `i & 63` inside a validated block.  Block layout: FS_BLOCK_ROWS
 * uint16 row byte lengths, then the rows.  Each row: LEB128 edge count, then
 * the Huffman bit stream (MSB first).  `*limit` is the furthest the read
 * pointer may legitimately get (the refill looks up to 4 bytes ahead). */
static inline const uint8_t *row_ptr(const uint8_t *blk, uint32_t i, uint32_t *count, const uint8_t **limit) {
    const uint16_t *len = (const uint16_t *)(const void *)blk;
    uint32_t r = i & (FS_BLOCK_ROWS - 1u);
    if (len[r] == 0) return NULL;
    uint32_t off = FS_BLOCK_ROWS * 2u;
    for (uint32_t k = 0; k < r; k++) off += len[k];
    const uint8_t *p = blk + off;
    *limit = p + len[r] + 4u;
    uint32_t c = 0, shift = 0, byte;
    do {
        byte = *p++;
        c |= (byte & 0x7Fu) << shift;
        shift += 7u;
    } while ((byte & 0x80u) && shift < 28u);
    *count = c;
    return p;
}

/* Symbol lengths and indices are validated when the tier is opened (see
 * lut_ok), so every shift below is by less than 32 and one refill always
 * covers the next field. */
#define DECODE_GAP(b, lut, t)                                                  \
    do {                                                                       \
        REFILL(b);                                                             \
        uint32_t e_ = (lut)[(b).buf >> (32u - FS_GAP_LUT_BITS)];               \
        uint32_t sym_ = e_ & 0xFFu;                                            \
        DROP(b, e_ >> 8);                                                      \
        if (sym_ < FS_GAP_EXACT) {                                             \
            (t) += sym_ + 1u;                                                  \
        } else {                                                               \
            uint32_t k_ = sym_ - FS_GAP_EXACT + 4u;                            \
            REFILL(b);                                                         \
            (t) += ((1u << k_) | ((b).buf >> (32u - k_))) + 1u;                \
            DROP(b, k_);                                                       \
        }                                                                      \
    } while (0)

/* Needs FS_W_LUT_BITS valid bits in the buffer on entry. */
#define DECODE_W_READY(b, lut, w)                                              \
    do {                                                                       \
        uint32_t e_ = (lut)[(b).buf >> (32u - FS_W_LUT_BITS)];                 \
        uint32_t sym_ = e_ & 0xFFu;                                            \
        DROP(b, e_ >> 8);                                                      \
        if (sym_ < FS_W_EXACT) {                                               \
            (w) = sym_;                                                        \
        } else {                                                               \
            uint32_t k_ = sym_ - FS_W_EXACT + 3u;                              \
            REFILL(b);                                                         \
            (w) = (1u << k_) | ((b).buf >> (32u - k_));                        \
            DROP(b, k_);                                                       \
        }                                                                      \
    } while (0)

#define DECODE_W(b, lut, w)                                                    \
    do {                                                                       \
        REFILL(b);                                                             \
        DECODE_W_READY(b, lut, w);                                             \
    } while (0)

/* A gap immediately followed by a weight, the inner loop of delivery.  A
 * refill leaves at least 25 bits; a gap code takes at most FS_GAP_LUT_BITS
 * of them, which still leaves a whole weight lookup, so the usual edge
 * (small gap, small weight) costs a single refill. */
typedef char fs_one_refill_covers_gap_and_weight[(25u - FS_GAP_LUT_BITS >= FS_W_LUT_BITS) ? 1 : -1];
#define DECODE_EDGE(b, glut, wlut, t, w)                                       \
    do {                                                                       \
        REFILL(b);                                                             \
        uint32_t ge_ = (glut)[(b).buf >> (32u - FS_GAP_LUT_BITS)];             \
        uint32_t gs_ = ge_ & 0xFFu;                                            \
        DROP(b, ge_ >> 8);                                                     \
        if (gs_ < FS_GAP_EXACT) {                                              \
            (t) += gs_ + 1u;                                                   \
        } else {                                                               \
            uint32_t gk_ = gs_ - FS_GAP_EXACT + 4u;                            \
            REFILL(b);                                                         \
            (t) += ((1u << gk_) | ((b).buf >> (32u - gk_))) + 1u;              \
            DROP(b, gk_);                                                      \
            REFILL(b);                                                         \
        }                                                                      \
        DECODE_W_READY(b, wlut, w);                                            \
    } while (0)

/* a + x without signed-overflow UB even if a data file lies about weights. */
#define G_ADD(a, x) ((int32_t)((uint32_t)(a) + (uint32_t)(x)))

/* ------------------------------------------------------ sleeping neurons */

/* (a * k) / 2^16, rounded toward zero so that state decays to exactly 0. */
static inline int32_t mulq16(int32_t a, int32_t k) {
    return (int32_t)(((int64_t)a * k) / 65536);
}

/* Could this neuron cross threshold with no further input?
 *
 * Without input one step maps (v, g) to (T(v dv) + T(g cv), T(g dg)), where
 * T rounds toward zero and the multipliers are Q16.  Let v+ = max(v, 0),
 * g+ = max(g, 0) and B = v+ + floor(g+ / 2^sh).  If (cv << sh) + dg <= 2^16
 * then one step cannot increase B:
 *
 *     v'+  <=  v+ + floor(g+ cv / 2^16)
 *     floor(g+ cv / 2^16) + floor(floor(g+ dg / 2^16) / 2^sh)
 *          <=  floor(g+ (cv + dg / 2^sh) / 2^16)  <=  floor(g+ / 2^sh)
 *
 * so v never exceeds today's B, and a neuron with B <= theta stays silent
 * until something new arrives.  fs_open picks the largest such sh (2 for
 * the standard constants).  If there is none, any positive drive keeps a
 * neuron awake instead; that needs only dv <= 2^16 and cv >= 0. */
static inline int may_fire(const fs_sim_t *s, int32_t v, int32_t g) {
    uint32_t vp = v > 0 ? (uint32_t)v : 0u, gp = g > 0 ? (uint32_t)g : 0u;
    if (s->bound_shift == FS_NO_BOUND) return gp != 0u || vp > (uint32_t)s->theta;
    return vp + (gp >> s->bound_shift) > (uint32_t)s->theta;
}

static void wake(fs_sim_t *s, uint32_t i) {
    const uint32_t w = i >> 5;
    s->cell[i].flags |= FS_CELL_AWAKE;
    s->awake[w] |= 1u << (i & 31u);
    s->awake_sum[w >> 5] |= 1u << (w & 31u);
    s->n_awake++;
}

/* |a| * k / 2^16 for a magnitude: what mulq16 does to the size of a number.
 * Rounding toward zero is rounding the magnitude down, so working on
 * magnitudes gives the same results with half the instructions. */
static inline uint32_t umulq16(uint32_t a, uint32_t k) {
    return (uint32_t)(((uint64_t)a * k) >> 16);
}

/* mulq16 written that way (k >= 0). */
static inline int32_t mulq16_mag(int32_t a, uint32_t k) {
    const uint32_t m = a < 0 ? ~0u : 0u; /* (x ^ m) - m negates when a < 0 */
    return (int32_t)((umulq16(((uint32_t)a ^ m) - m, k) ^ m) - m);
}

/* Replay k input-free steps of a sleeping neuron (no timer running, and by
 * may_fire() no spike possible): the arithmetic of the per-tick pass of
 * fs_step, in the same order.  The drive keeps its sign while it decays,
 * so it is carried as a magnitude.  catchup_steps counts loop iterations,
 * i.e. work: steps skipped by the shortcut below are not in it. */
static void decay_missed(fs_sim_t *s, fs_cell_t *c, uint32_t k) {
    const uint32_t dv = (uint32_t)s->dv, dg = (uint32_t)s->dg, cv = (uint32_t)s->cv;
    const uint32_t k0 = k;
    int32_t v = c->v;
    const int32_t g0 = c->g;
    const uint32_t neg = g0 < 0 ? ~0u : 0u;
    uint32_t ug = ((uint32_t)g0 ^ neg) - neg;
    /* k >= 1 on entry; each loop is shaped as "test the state, step, count
     * down" because that is two short branches on the ARM. */
    if (c->flags & FS_CELL_SILENCED) {
        v = 0;
        while (ug) {
            ug = umulq16(ug, dg);
            if (--k == 0) break;
        }
    } else {
        while (ug) {
            v = G_ADD(mulq16_mag(v, dv), (umulq16(ug, cv) ^ neg) - neg);
            ug = umulq16(ug, dg);
            if (--k == 0) break;
        }
        /* Only v is left.  Close to zero the rounding takes exactly one
         * unit off per step, which needs no loop. */
        const int32_t lin = s->v_linear;
        while (k && v) {
            if (v <= lin && v >= -lin) {
                if (v > 0) v = (uint32_t)v > k ? v - (int32_t)k : 0;
                else v = (uint32_t)(-v) > k ? v + (int32_t)k : 0;
                break;
            }
            v = mulq16_mag(v, dv);
            k--;
        }
    }
    s->catchup_steps += k0 - k;
    c->v = v;
    c->g = (int32_t)((ug ^ neg) - neg);
}

/* Make a neuron's state that of the start of step `now`.  Awake neurons
 * always are; a sleeping one is brought up to date the first time it is
 * touched in a step. */
static inline void bring_current(fs_sim_t *s, fs_cell_t *c, uint32_t now) {
    if (c->stamp == now) return;
    if (c->v | c->g) decay_missed(s, c, now - c->stamp);
    c->stamp = now;
}

/* Add `x` to a neuron's synaptic drive, waking it if that puts threshold
 * within reach. */
#define SYNAPSE(s, cells, tgt, now, x)                                              \
    do {                                                                            \
        fs_cell_t *c_ = &(cells)[tgt];                                              \
        bring_current(s, c_, now);                                                  \
        int32_t g_ = G_ADD(c_->g, x);                                               \
        c_->g = g_;                                                                 \
        if (g_ > 0 && !(c_->flags & (FS_CELL_AWAKE | FS_CELL_SILENCED)) &&          \
            may_fire(s, c_->v, g_))                                                 \
            wake(s, tgt);                                                           \
    } while (0)

/* Add one spike of neuron i to all its targets in tier t.  A stream that
 * runs past its row or names a neuron that does not exist is corrupt: stop
 * and count it, never follow it. */
static uint32_t deliver(fs_sim_t *s, fs_tier_t *t, uint32_t i, int32_t unit) {
    const uint8_t *blk = block_get(s, t, i >> FS_BLOCK_SHIFT);
    if (!blk) {
        s->io_errors++;
        return 0;
    }
    uint32_t count = 0;
    const uint8_t *limit = NULL;
    bits_t b;
    b.p = row_ptr(blk, i, &count, &limit);
    if (!b.p) return 0;
    b.buf = 0;
    b.cnt = 0;
    const uint16_t *glut = t->desc->gap_lut;
    fs_cell_t *cells = s->cell;
    uint32_t tgt = (uint32_t)-1;
    const uint32_t n = s->n, now = s->step;
    uint32_t e = count;
    if (t->desc->has_weights) {
        const uint16_t *wlut = t->desc->w_lut;
        const uint32_t wmin = t->desc->wmin;
        for (; e; e--) {
            uint32_t w;
            DECODE_EDGE(b, glut, wlut, tgt, w);
            if (tgt >= n || b.p > limit) break;
            SYNAPSE(s, cells, tgt, now, unit * (int32_t)(w + wmin));
        }
    } else {
        const int32_t add = unit * (int32_t)t->desc->wmin;
        for (; e; e--) {
            DECODE_GAP(b, glut, tgt);
            if (tgt >= n || b.p > limit) break;
            SYNAPSE(s, cells, tgt, now, add);
        }
    }
    if (e) s->io_errors++;
    return count - e;
}

int32_t fs_decode_row(fs_sim_t *s, unsigned tier, uint32_t neuron, uint32_t *targets, uint32_t *weights,
                      uint32_t cap) {
    if (tier >= s->hdr->n_tiers || neuron >= s->n) return FS_ERR_ARG;
    fs_tier_t *t = &s->tier[tier];
    const uint8_t *blk = block_get(s, t, neuron >> FS_BLOCK_SHIFT);
    if (!blk) {
        s->io_errors++;
        return FS_ERR_IO;
    }
    uint32_t count = 0;
    const uint8_t *limit = NULL;
    bits_t b;
    b.p = row_ptr(blk, neuron, &count, &limit);
    if (!b.p) return 0;
    if (count > cap) return FS_ERR_ARG;
    b.buf = 0;
    b.cnt = 0;
    uint32_t tgt = (uint32_t)-1;
    for (uint32_t e = 0; e < count; e++) {
        uint32_t w = 0;
        DECODE_GAP(b, t->desc->gap_lut, tgt);
        if (t->desc->has_weights) DECODE_W(b, t->desc->w_lut, w);
        if (tgt >= s->n || b.p > limit) return FS_ERR_FORMAT;
        targets[e] = tgt;
        weights[e] = w + t->desc->wmin;
    }
    return (int32_t)count;
}

/* Decode (and optionally apply) the rows of `count` neurons starting at
 * `first`, pinned blocks only, so it never touches storage.  For measuring
 * decode speed on the target; apply != 0 disturbs the neural state, so call
 * fs_reset() afterwards.  Returns the number of edges visited. */
uint32_t fs_bench_rows(fs_sim_t *s, uint32_t first, uint32_t count, int apply) {
    uint32_t edges = 0, sink = 0;
    for (uint32_t i = first; i < s->n && i - first < count; i++) {
        for (unsigned k = 0; k < s->hdr->n_tiers; k++) {
            fs_tier_t *t = &s->tier[k];
            if (t->where[i >> FS_BLOCK_SHIFT] != FS_AT_PINNED) continue;
            if (apply) {
                edges += deliver(s, t, i, 1 << FS_FRAC);
                continue;
            }
            uint32_t n_edges = 0;
            const uint8_t *limit = NULL;
            bits_t b;
            b.p = row_ptr(t->block[i >> FS_BLOCK_SHIFT], i, &n_edges, &limit);
            if (!b.p) continue;
            b.buf = 0;
            b.cnt = 0;
            uint32_t tgt = (uint32_t)-1;
            for (uint32_t e = n_edges; e; e--) {
                uint32_t w = 0;
                DECODE_GAP(b, t->desc->gap_lut, tgt);
                if (t->desc->has_weights) DECODE_W(b, t->desc->w_lut, w);
                if (tgt >= s->n || b.p > limit) break;
                sink += tgt + w;
                edges++;
            }
        }
    }
    s->bench_sink = sink; /* keep the decode from being optimised away */
    return edges;
}

/* Give every neuron drive g and wake it, so that the next fs_step
 * integrates all of them once. */
void fs_bench_wake_all(fs_sim_t *s, int32_t g) {
    for (uint32_t i = 0; i < s->n; i++) {
        fs_cell_t *c = &s->cell[i];
        bring_current(s, c, s->step);
        c->g = g;
        if (!(c->flags & FS_CELL_AWAKE)) wake(s, i);
    }
}

/* Reset, put every neuron to sleep `gap` steps in the past with drive g,
 * and bring them all up to date.  Returns the replay loop iterations this
 * took (0 if g is large enough that such a neuron would not be asleep). */
uint64_t fs_bench_catch_up(fs_sim_t *s, int32_t g, uint32_t gap) {
    fs_reset(s);
    if (may_fire(s, 0, g)) return 0;
    for (uint32_t i = 0; i < s->n; i++) {
        s->cell[i].g = g;
        s->cell[i].stamp = s->step - gap;
    }
    for (uint32_t i = 0; i < s->n; i++) bring_current(s, &s->cell[i], s->step);
    return s->catchup_steps;
}

/* ------------------------------------------------------------------ step */

static inline uint32_t xorshift32(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static inline uint32_t lowest_set(uint32_t w) { return (uint32_t)__builtin_ctz(w); }

void fs_step(fs_sim_t *s) {
    const uint32_t n = s->n, ns = s->n_sum, now = s->step;
    fs_cell_t *cells = s->cell;
    uint32_t *awake = s->awake, *sum = s->awake_sum;
    const uint8_t *flags = s->flags;
    const uint32_t tick0 = s->ticks ? s->ticks() : 0u;

    /* Pass A: deliver spikes whose synaptic delay has elapsed.  Every
     * neuron with a running timer is awake, and the awake ones are found
     * through a two-level bitmap, so a quiet network costs almost nothing.
     * A delivery can wake neurons while this loop runs; whether or not it
     * then comes across them does not matter, because their timers are
     * idle. */
    const uint8_t due = s->timer_deliver;
    uint64_t edges = 0;
    for (uint32_t j = 0; j < ns; j++) {
        for (uint32_t sw = sum[j]; sw; sw &= sw - 1u) {
            const uint32_t k = (j << 5) + lowest_set(sw);
            for (uint32_t w = awake[k]; w; w &= w - 1u) {
                const uint32_t i = (k << 5) + lowest_set(w);
                if (cells[i].tmr != due) continue;
                uint32_t sign = flags[i] & FS_SIGN_MASK;
                if (sign == FS_SIGN_NONE) continue;
                int32_t unit = (sign == FS_SIGN_INH) ? -(1 << FS_FRAC) : (1 << FS_FRAC);
                for (unsigned tier = 0; tier < s->hdr->n_tiers; tier++)
                    if (s->tier[tier].enabled) edges += deliver(s, &s->tier[tier], i, unit);
            }
        }
    }
    s->total_edges += edges;
    const uint32_t tick1 = s->ticks ? s->ticks() : 0u;

    /* Stimulus: one random draw per stimulated neuron per step.  An event
     * forces a spike this step (as Shiu et al. kick v far above threshold),
     * unless the neuron is still inside its previous spike's timer window. */
    uint32_t rng = s->rng;
    for (unsigned k = 0; k < FS_MAX_STIM; k++) {
        const fs_stim_t *st = &s->stim[k];
        for (uint32_t j = 0; j < st->n; j++) {
            rng = xorshift32(rng);
            if (rng >= st->thresh) continue;
            uint32_t i = st->ids[j];
            if (i >= n) continue;
            fs_cell_t *c = &cells[i];
            if (c->tmr != 0 || (c->flags & FS_CELL_SILENCED)) continue;
            bring_current(s, c, now);
            c->v = FS_V_FORCE;
            if (!(c->flags & FS_CELL_AWAKE)) wake(s, i);
        }
    }
    s->rng = rng;

    /* Pass B: integrate the awake neurons in index order, detect spikes,
     * and send back to sleep those that are out of reach of threshold. */
    const int32_t dv = s->dv, dg = s->dg, cv = s->cv, theta = s->theta;
    const uint8_t t_start = s->timer_start, t_refr = s->timer_refrac_min;
    uint32_t hash = s->hash, n_spk = 0, updates = 0, n_awake = s->n_awake;
    for (uint32_t j = 0; j < ns; j++) {
        uint32_t sw = sum[j];
        if (!sw) continue;
        uint32_t sum_keep = sw;
        do {
            const uint32_t sbit = sw & (0u - sw);
            const uint32_t k = (j << 5) + lowest_set(sw);
            sw ^= sbit;
            uint32_t w = awake[k], keep = w;
            while (w) {
                const uint32_t i = (k << 5) + lowest_set(w);
                const uint32_t bit = w & (0u - w);
                w ^= bit;
                fs_cell_t *c = &cells[i];
                uint32_t t = c->tmr;
                int32_t gi = c->g, vn = c->v;
                const uint32_t fl = c->flags;
                int refractory = 0;
                if (t) {
                    refractory = t >= t_refr;
                    t--;
                }
                if (fl & FS_CELL_SILENCED) {
                    /* Silenced: input still decays, but it can never spike. */
                    vn = 0;
                    gi = mulq16(gi, dg);
                } else if (!refractory) {
                    vn = G_ADD(mulq16(vn, dv), mulq16(gi, cv)); /* wraps like decay_missed and the mirror */
                    gi = mulq16(gi, dg);
                    if (vn > theta) {
                        vn = 0;
                        gi = 0; /* the spike resets the synaptic variable too */
                        t = t_start;
                        hash = (hash ^ i) * 16777619u;
                        hash = (hash ^ now) * 16777619u;
                        if (n_spk < FS_SPIKE_CAP) s->spikes[n_spk] = i;
                        n_spk++;
                        s->type_count[s->type[i]]++;
                        s->group_count[s->group[i]]++;
                    }
                } /* else refractory: v and g are frozen; arriving input keeps adding to g */
                c->v = vn;
                c->g = gi;
                c->stamp = now + 1u;
                c->tmr = (uint8_t)t;
                updates++;
                if (!t && ((fl & FS_CELL_SILENCED) || !may_fire(s, vn, gi))) {
                    c->flags = (uint8_t)(fl & ~FS_CELL_AWAKE);
                    keep ^= bit;
                    n_awake--;
                }
            }
            if (keep != awake[k]) awake[k] = keep;
            if (!keep) sum_keep ^= sbit;
        } while (sw);
        if (sum_keep != sum[j]) sum[j] = sum_keep;
    }
    s->n_awake = n_awake;
    s->awake_updates += updates;
    s->hash = hash;
    s->n_spikes = n_spk;
    s->n_listed = n_spk < FS_SPIKE_CAP ? n_spk : FS_SPIKE_CAP;
    s->total_spikes += n_spk;
    s->step = now + 1u;
    if (s->ticks) {
        const uint32_t tick2 = s->ticks();
        s->ticks_deliver += (uint32_t)(tick1 - tick0);
        s->ticks_integrate += (uint32_t)(tick2 - tick1);
    }
}

/* ---------------------------------------------------------------- control */

void fs_set_gain(fs_sim_t *s, uint32_t w_syn_uv) {
    if (w_syn_uv == 0) return;
    const int32_t old = s->theta;
    s->w_syn_uv = w_syn_uv;
    s->theta = (int32_t)((((uint64_t)s->hdr->v_thresh_uv << FS_FRAC) + w_syn_uv / 2u) / w_syn_uv);
    if (!s->cell || s->theta >= old) return;
    /* The threshold came down: sleeping neurons may now be within reach of
     * it.  Their past is settled (they could not fire under the old one). */
    for (uint32_t i = 0; i < s->n; i++) {
        fs_cell_t *c = &s->cell[i];
        if ((c->flags & (FS_CELL_AWAKE | FS_CELL_SILENCED)) || !(c->v | c->g)) continue;
        /* The bound only shrinks while a neuron sleeps, so one that is out
         * of reach on its stored state is out of reach now. */
        if (!may_fire(s, c->v, c->g)) continue;
        bring_current(s, c, s->step);
        if (may_fire(s, c->v, c->g)) wake(s, i);
    }
}

void fs_set_silenced(fs_sim_t *s, uint32_t neuron, int silenced) {
    if (neuron >= s->n) return;
    fs_cell_t *c = &s->cell[neuron];
    if (((c->flags & FS_CELL_SILENCED) != 0) == (silenced != 0)) return;
    bring_current(s, c, s->step); /* settle the past under the old rule */
    c->flags ^= FS_CELL_SILENCED;
    if (!silenced && !(c->flags & FS_CELL_AWAKE) && may_fire(s, c->v, c->g)) wake(s, neuron);
}

int fs_is_silenced(const fs_sim_t *s, uint32_t neuron) {
    return neuron < s->n && (s->cell[neuron].flags & FS_CELL_SILENCED) != 0;
}

void fs_set_stim(fs_sim_t *s, unsigned slot, const uint32_t *ids, uint32_t n, uint32_t rate_hz) {
    if (slot >= FS_MAX_STIM) return;
    /* A stimulated neuron cannot fire again for timer_start steps, so the
     * per-step probability is raised to p = r / (1 - dead * r) to make the
     * mean rate come out at rate_hz (saturating at one spike per window). */
    uint64_t num = (uint64_t)rate_hz * s->hdr->dt_us; /* r, in millionths per step */
    uint64_t dead = (uint64_t)s->timer_start * num;
    uint64_t th = 0xFFFFFFFFull;
    if (dead < 1000000ull) {
        uint64_t den = 1000000ull - dead;
        th = ((num << 32) + den / 2u) / den;
        if (th > 0xFFFFFFFFull) th = 0xFFFFFFFFull;
    }
    s->stim[slot].ids = ids;
    s->stim[slot].n = ids ? n : 0;
    s->stim[slot].thresh = (uint32_t)th;
}

void fs_clear_stims(fs_sim_t *s) { memset(s->stim, 0, sizeof s->stim); }

void fs_clear_counts(fs_sim_t *s) {
    memset(s->type_count, 0, (size_t)s->hdr->n_types * sizeof(uint32_t));
    memset(s->group_count, 0, (size_t)s->hdr->n_groups * sizeof(uint32_t));
}

void fs_reset(fs_sim_t *s) {
    for (uint32_t i = 0; i < s->n; i++) {
        fs_cell_t *c = &s->cell[i];
        c->v = 0;
        c->g = 0;
        c->stamp = 0;
        c->tmr = 0;
        c->flags &= FS_CELL_SILENCED; /* silencing is a setting, not state */
    }
    memset(s->awake, 0, (size_t)s->n_words * sizeof(uint32_t));
    memset(s->awake_sum, 0, (size_t)s->n_sum * sizeof(uint32_t));
    s->n_awake = 0;
    s->ticks_deliver = 0;
    s->ticks_integrate = 0;
    s->ticks_fault = 0;
    fs_clear_counts(s);
    s->rng = s->hdr->seed;
    s->step = 0;
    s->total_spikes = 0;
    s->total_edges = 0;
    s->awake_updates = 0;
    s->catchup_steps = 0;
    s->hash = 2166136261u;
    s->n_spikes = 0;
    s->n_listed = 0;
}

/* ------------------------------------------------------------------- open */

static int load_meta(fs_sim_t *s, const char *dir, const char *meta_name) {
    FILE *f = open_in_dir(dir, meta_name);
    if (!f) return FS_ERR_IO;
    int rc = FS_ERR_IO;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size >= (long)sizeof(fs_header_t) && size < (64L << 20) && fseek(f, 0, SEEK_SET) == 0) {
        s->meta = malloc((size_t)size);
        if (!s->meta) {
            rc = FS_ERR_MEMORY;
        } else if (fread(s->meta, 1, (size_t)size, f) == (size_t)size) {
            s->meta_bytes = (uint32_t)size;
            rc = FS_OK;
        }
    }
    fclose(f);
    return rc;
}

static int check_meta(fs_sim_t *s) {
    const fs_header_t *h = (const fs_header_t *)(const void *)s->meta;
    if (memcmp(h->magic, FS_MAGIC, 8) != 0 || h->version != FS_VERSION) return FS_ERR_FORMAT;
    if (h->n_tiers == 0 || h->n_tiers > FS_MAX_TIERS) return FS_ERR_FORMAT;
    if (h->n_neurons == 0 || h->n_neurons >= (1u << 18)) return FS_ERR_FORMAT;
    if (h->n_types == 0 || h->n_types > 65536u || h->n_groups == 0 || h->n_groups > 256u) return FS_ERR_FORMAT;
    if (h->n_blocks != (h->n_neurons + FS_BLOCK_ROWS - 1u) / FS_BLOCK_ROWS) return FS_ERR_FORMAT;
    if (h->delay == 0 || h->refrac == 0 || h->delay > 200u || h->refrac > 200u) return FS_ERR_FORMAT;
    if (h->dt_us < 100u || h->dt_us > 100000u || h->seed == 0) return FS_ERR_FORMAT;
    if (h->v_thresh_uv == 0 || h->v_thresh_uv > 1000000u) return FS_ERR_FORMAT;
    if (h->w_syn_uv == 0 || h->w_syn_uv > 100000u) return FS_ERR_FORMAT;
    /* Strictly below 1: state must decay, or catching up a sleeping neuron never ends. */
    if (h->dv <= 0 || h->dv >= 65536 || h->dg <= 0 || h->dg >= 65536 || h->cv < 0 || h->cv > 65536) return FS_ERR_FORMAT;
    uint64_t dir_end = sizeof(fs_header_t) + (uint64_t)h->n_sections * sizeof(fs_section_t);
    if (dir_end > s->meta_bytes) return FS_ERR_FORMAT;
    const fs_section_t *sec = (const fs_section_t *)(s->meta + sizeof(fs_header_t));
    for (uint32_t i = 0; i < h->n_sections; i++) {
        if ((sec[i].offset & 3u) || (uint64_t)sec[i].offset + sec[i].size > s->meta_bytes) return FS_ERR_FORMAT;
    }
    s->hdr = h;
    s->n = h->n_neurons;
    s->n_blocks = h->n_blocks;
    s->flags = need_section(s, "NFLG", s->n);
    s->pix = need_section(s, "NPIX", s->n * 2u);
    s->type = need_section(s, "NTYP", s->n * 2u);
    s->group = need_section(s, "NGRP", s->n);
    if (!s->flags || !s->pix || !s->type || !s->group) return FS_ERR_FORMAT;
    for (uint32_t i = 0; i < s->n; i++) {
        if (s->type[i] >= h->n_types || s->group[i] >= h->n_groups) return FS_ERR_FORMAT;
    }
    return FS_OK;
}

/* Every lookup-table entry is (code length << 8) | symbol.  Bounding both
 * here is what makes the decoder's shifts and refills safe. */
static int lut_ok(const uint16_t *lut, uint32_t entries, uint32_t max_len, uint32_t n_syms) {
    for (uint32_t i = 0; i < entries; i++)
        if ((uint32_t)(lut[i] >> 8) > max_len || (uint32_t)(lut[i] & 0xFFu) >= n_syms) return 0;
    return 1;
}

/* Optional section ranking every (tier, block) unit from most to least used
 * in the built-in experiments, coded tier << 15 | block; it decides what to
 * keep in RAM when not everything fits.  Ignored unless it is a permutation
 * of all units. */
static const uint16_t *hot_order(const fs_sim_t *s) {
    const uint32_t units = s->hdr->n_tiers * s->n_blocks;
    const uint16_t *hot = need_section(s, "HOTS", units * 2u);
    if (!hot || s->n_blocks > 0x8000u) return NULL;
    for (unsigned k = 0; k < s->hdr->n_tiers; k++) memset(s->tier[k].where, 0, s->n_blocks);
    for (uint32_t i = 0; i < units && hot; i++) {
        uint32_t tier = hot[i] >> 15, b = hot[i] & 0x7FFFu;
        if (tier >= s->hdr->n_tiers || b >= s->n_blocks || s->tier[tier].where[b]) hot = NULL;
        else s->tier[tier].where[b] = 1; /* scratch: seen */
    }
    for (unsigned k = 0; k < s->hdr->n_tiers; k++) memset(s->tier[k].where, FS_AT_FLASH, s->n_blocks);
    return hot;
}

static int open_tiers(fs_sim_t *s, const char *dir) {
    static const char *const tags[FS_MAX_TIERS] = {"TIR0", "TIR1"};
    const uint32_t dir_bytes = (s->n_blocks + 1u) * 4u;
    s->total_bytes = 0;
    for (unsigned k = 0; k < s->hdr->n_tiers; k++) {
        fs_tier_t *t = &s->tier[k];
        const fs_tier_desc_t *d = need_section(s, tags[k], (uint32_t)sizeof(fs_tier_desc_t) + dir_bytes);
        if (!d || memchr(d->file, 0, sizeof d->file) == NULL) return FS_ERR_FORMAT;
        if (d->wmin == 0 || d->wmin > 65535u) return FS_ERR_FORMAT;
        if (!lut_ok(d->gap_lut, 1u << FS_GAP_LUT_BITS, FS_GAP_LUT_BITS, FS_GAP_SYMS) ||
            !lut_ok(d->w_lut, 1u << FS_W_LUT_BITS, FS_W_LUT_BITS, FS_W_SYMS))
            return FS_ERR_FORMAT;
        t->desc = d;
        t->block_off = (const uint32_t *)(const void *)((const uint8_t *)d + sizeof(fs_tier_desc_t));
        if (t->block_off[0] != 0) return FS_ERR_FORMAT;
        for (uint32_t b = 0; b < s->n_blocks; b++) {
            if (t->block_off[b + 1] < t->block_off[b]) return FS_ERR_FORMAT;
            uint32_t size = t->block_off[b + 1] - t->block_off[b];
            if (size < FS_BLOCK_ROWS * 2u || size > FS_MAX_BLOCK_BYTES) return FS_ERR_FORMAT;
        }
        if (t->block_off[s->n_blocks] != d->file_bytes) return FS_ERR_FORMAT;
        t->fp = open_in_dir(dir, d->file);
        if (!t->fp) return FS_ERR_IO;
        /* A truncated file would otherwise only show up as lost synapses. */
        if (fseek(t->fp, 0, SEEK_END) != 0 || ftell(t->fp) != (long)d->file_bytes) return FS_ERR_FORMAT;
        t->block = calloc(s->n_blocks, sizeof(uint8_t *));
        t->where = calloc(s->n_blocks, 1);
        if (!t->block || !t->where) return FS_ERR_MEMORY;
        t->enabled = 1;
        s->total_bytes += d->file_bytes;
    }
    s->hot = hot_order(s);
    return FS_OK;
}

/* Grab chunks until we have enough for everything, hit the cap, or
 * malloc refuses.  Then make sure `reserve_bytes` are still obtainable (the
 * OS needs room for its file system and USB link), handing chunks back if
 * they are not. */
static int alloc_arena(fs_sim_t *s, uint32_t max_arena_bytes, uint32_t reserve_bytes) {
    uint32_t want = s->total_bytes / FS_CHUNK_BYTES + 1u;
    while (want < FS_MAX_CHUNKS && !everything_fits(s, want)) want++;
    if (max_arena_bytes) {
        uint32_t cap = max_arena_bytes / FS_CHUNK_BYTES;
        if (cap < want) want = cap;
    }
    while (s->n_chunks < want) {
        uint8_t *p = malloc(FS_CHUNK_BYTES);
        if (!p) {
            s->arena_exhausted = 1;
            break;
        }
        s->chunk[s->n_chunks++] = p;
    }
    uint32_t reserve = (reserve_bytes + FS_CHUNK_BYTES - 1u) / FS_CHUNK_BYTES;
    if (reserve > FS_MAX_CHUNKS) reserve = FS_MAX_CHUNKS;
    uint8_t *probe[FS_MAX_CHUNKS];
    uint32_t got = 0;
    while (got < reserve && (probe[got] = malloc(FS_CHUNK_BYTES)) != NULL) got++;
    for (uint32_t k = 0; k < got; k++) free(probe[k]);
    for (uint32_t missing = reserve - got; missing && s->n_chunks; missing--) free(s->chunk[--s->n_chunks]);
    return s->n_chunks >= 1u ? FS_OK : FS_ERR_MEMORY;
}

/* Split the arena into a pinned region and a paging pool.  If everything
 * fits there is no pool.  Otherwise the pool gets an eighth of the arena
 * (at least 2 MB when the arena allows), as two half-spaces when there is
 * room for two.  Page-ins from flash are slow, so most memory is pinned. */
static void plan_pool(fs_sim_t *s) {
    uint32_t n = s->n_chunks, pool = 0;
    if (!everything_fits(s, n)) {
        pool = n / 8u;
        if (pool < FS_MIN_POOL_CHUNKS) pool = FS_MIN_POOL_CHUNKS;
        if (pool > n / 2u) pool = n / 2u; /* tiny arena: still pin half */
        if (pool < 2u) pool = n < 2u ? n : 2u;
    }
    s->pool_first = n - pool;
    uint32_t half = pool / 2u;
    s->space_first[0] = s->pool_first;
    s->space_end[0] = n - half;
    s->space_first[1] = n - half;
    s->space_end[1] = n;
    if (pool < 2u) { /* zero or one chunk: a single space, flushed when full */
        s->space_end[0] = n;
        s->space_first[1] = n;
    }
    s->active = 0;
    s->pool_chunk = s->space_first[0];
    s->pool_off = 0;
}

/* Give pinned-region addresses to as many blocks as fit: by measured usage
 * if the data carries a ranking and something will have to be paged,
 * otherwise in file order (the order everything_fits() assumed). */
static void choose_pinned(fs_sim_t *s) {
    const uint32_t units = s->hdr->n_tiers * s->n_blocks;
    const int ranked = s->hot && s->pool_first < s->n_chunks;
    for (uint32_t i = 0; i < units; i++) {
        unsigned tier = ranked ? (unsigned)(s->hot[i] >> 15) : (unsigned)(i / s->n_blocks);
        uint32_t b = ranked ? (uint32_t)(s->hot[i] & 0x7FFFu) : i % s->n_blocks;
        fs_tier_t *t = &s->tier[tier];
        uint8_t *p = bump(s, &s->fill_chunk, &s->fill_off, s->pool_first, t->block_off[b + 1] - t->block_off[b]);
        if (!p) continue; /* too big for what is left: a smaller block may still fit */
        t->block[b] = p;
        t->where[b] = FS_AT_PINNED; /* provisional until preload has read it */
    }
}

/* Read the chosen blocks in file order. */
static int preload(fs_sim_t *s, fs_progress_fn progress, void *ctx) {
    plan_pool(s);
    choose_pinned(s);
    uint32_t last_report = 0;
    for (unsigned k = 0; k < s->hdr->n_tiers; k++) {
        fs_tier_t *t = &s->tier[k];
        for (uint32_t b = 0; b < s->n_blocks; b++) {
            if (t->where[b] != FS_AT_PINNED) continue;
            int rc = read_block(t, b, t->block[b]);
            if (rc != FS_OK) return rc;
            s->resident_bytes += t->block_off[b + 1] - t->block_off[b];
            if (progress && s->resident_bytes - last_report >= (256u << 10)) {
                progress(ctx, s->resident_bytes, s->total_bytes);
                last_report = s->resident_bytes;
            }
        }
    }
    if (progress) progress(ctx, s->resident_bytes, s->total_bytes);
    /* Without a pool nothing can be paged: every block must be in RAM. */
    if (s->pool_first == s->n_chunks && s->resident_bytes != s->total_bytes) return FS_ERR_MEMORY;
    return FS_OK;
}

int fs_open(fs_sim_t *s, const char *dir, const char *meta_name, uint32_t max_arena_bytes,
            uint32_t reserve_bytes, fs_progress_fn progress, void *ctx) {
    memset(s, 0, sizeof *s);
    int rc = load_meta(s, dir, meta_name);
    if (rc == FS_OK) rc = check_meta(s);
    if (rc != FS_OK) goto fail;

    const fs_header_t *h = s->hdr;
    s->n_words = (s->n + 31u) >> 5;
    s->cell_mem = calloc((size_t)s->n + CELL_ALIGN / sizeof(fs_cell_t), sizeof(fs_cell_t));
    s->n_sum = (s->n_words + 31u) >> 5;
    s->awake = calloc(s->n_words, sizeof(uint32_t));
    s->awake_sum = calloc(s->n_sum, sizeof(uint32_t));
    s->type_count = calloc(h->n_types ? h->n_types : 1u, sizeof(uint32_t));
    s->group_count = calloc(h->n_groups ? h->n_groups : 1u, sizeof(uint32_t));
    if (!s->cell_mem || !s->awake || !s->awake_sum || !s->type_count || !s->group_count) {
        rc = FS_ERR_MEMORY;
        goto fail;
    }
    s->cell = (fs_cell_t *)(void *)(((uintptr_t)s->cell_mem + (CELL_ALIGN - 1u)) & ~(uintptr_t)(CELL_ALIGN - 1u));
    s->dv = h->dv;
    s->dg = h->dg;
    s->cv = h->cv;
    s->bound_shift = FS_NO_BOUND;
    for (uint32_t sh = MAX_BOUND_SHIFT + 1u; sh-- > 0u;) {
        if ((((uint64_t)(uint32_t)h->cv) << sh) + (uint32_t)h->dg <= 65536u) {
            s->bound_shift = sh;
            break;
        }
    }
    s->v_linear = 65535 / (65536 - h->dv);
    if (h->w_syn_uv == 0) {
        rc = FS_ERR_FORMAT;
        goto fail;
    }
    fs_set_gain(s, h->w_syn_uv);
    uint32_t start = (h->delay > h->refrac ? h->delay : h->refrac) + 1u;
    s->timer_start = (uint8_t)start;
    s->timer_deliver = (uint8_t)(start - h->delay + 1u);
    s->timer_refrac_min = (uint8_t)(start - h->refrac + 1u);

    rc = open_tiers(s, dir);
    if (rc == FS_OK) rc = alloc_arena(s, max_arena_bytes, reserve_bytes);
    if (rc == FS_OK) rc = preload(s, progress, ctx);
    if (rc != FS_OK) goto fail;
    fs_reset(s);
    return FS_OK;
fail:
    fs_close(s);
    return rc;
}

void fs_close(fs_sim_t *s) {
    for (unsigned k = 0; k < FS_MAX_TIERS; k++) {
        if (s->tier[k].fp) fclose(s->tier[k].fp);
        free(s->tier[k].block);
        free(s->tier[k].where);
    }
    for (uint32_t c = 0; c < s->n_chunks; c++) free(s->chunk[c]);
    free(s->cell_mem);
    free(s->awake);
    free(s->awake_sum);
    free(s->type_count);
    free(s->group_count);
    free(s->meta);
    memset(s, 0, sizeof *s);
}
