/* The on-screen fly: a top-view cartoon whose parts move in proportion to
 * the firing of the motor-neuron pool that drives them in the real animal.
 * It is a readout, not a biomechanical model. */
#include "app.h"

#define C_SHADOW RGB565(18, 22, 34)
#define C_HEAD RGB565(120, 92, 64)
#define C_EYE RGB565(205, 50, 45)
#define C_THORAX RGB565(134, 104, 70)
#define C_ABDOMEN RGB565(156, 122, 72)
#define C_STRIPE RGB565(84, 62, 40)
#define C_LEG RGB565(150, 150, 150)
#define C_LEG_HOT RGB565(255, 255, 255)
#define C_WING RGB565(120, 160, 200)
#define C_WING_HOT RGB565(255, 240, 120)
#define C_TONGUE RGB565(255, 120, 200)

/* sin(2*pi*i/64) * 256 */
static const int16_t SIN64[64] = {
    0, 25, 50, 74, 98, 121, 142, 162, 181, 198, 213, 226, 237, 245, 251, 255,
    256, 255, 251, 245, 237, 226, 213, 198, 181, 162, 142, 121, 98, 74, 50, 25,
    0, -25, -50, -74, -98, -121, -142, -162, -181, -198, -213, -226, -237, -245, -251, -255,
    -256, -255, -251, -245, -237, -226, -213, -198, -181, -162, -142, -121, -98, -74, -50, -25,
};

static int isin(int a) { return SIN64[a & 63]; }
static int icos(int a) { return SIN64[(a + 16) & 63]; }

/* Pool firing rate as 0..256 of `full_hz`. */
static int level(const app_t *a, int group, unsigned full_hz) {
    uint32_t l = a->rate_x16[group] * 256u / (full_hz * 16u);
    return l > 256u ? 256 : (int)l;
}

/* Angles are in 1/64 turns, 0 = toward the tail (+y), positive = outward. */
static void limb(app_t *a, int x, int y, int dir, int angle, int len, uint16_t c, int *ex, int *ey) {
    int tx = x + dir * (isin(angle) * len) / 256;
    int ty = y + (icos(angle) * len) / 256;
    gfx_line(&a->gfx, x, y, tx, ty, c);
    if (ex) *ex = tx;
    if (ey) *ey = ty;
}

static void leg(app_t *a, int x, int y, int dir, int rest, int lvl, int phase) {
    /* Swing amplitude grows with the pool's firing; tripod phase offsets. */
    int swing = (lvl * 5 * isin((int)a->anim * 5 + phase)) / (256 * 256);
    uint16_t c = lvl > 24 ? C_LEG_HOT : C_LEG;
    int kx, ky;
    limb(a, x, y, dir, rest + swing, 11, c, &kx, &ky);
    limb(a, kx, ky, dir, rest + swing + 9 - (lvl * 4) / 256, 12, c, NULL, NULL);
}

static void wing(app_t *a, int x, int y, int dir, int lvl, int flight) {
    /* Folded along the abdomen at rest; swings out as wing motor neurons
     * fire, with a flutter that stands in for song / wingbeat. */
    int open = lvl > flight ? lvl : flight;
    int flutter = open > 16 ? (isin((int)a->anim * 23) * (open > 128 ? 2 : 1)) / 256 : 0;
    int angle = 2 + (open * 14) / 256 + flutter;
    uint16_t c = open > 24 ? C_WING_HOT : C_WING;
    for (int d = -2; d <= 2; d++) limb(a, x, y, dir, angle + d / 2, 30 - (d < 0 ? -d : d) * 3, c, NULL, NULL);
}

void app_draw_fly(app_t *a, int cx, int cy) {
    gfx_t *g = &a->gfx;
    int jump = level(a, G_JUMP, 40);
    int hop = (jump * 7) / 256;
    gfx_ellipse(g, cx, cy + 6, 13 - hop / 2, 30 - hop, C_SHADOW);
    cy -= hop;

    int neck = level(a, G_NECK, 60);
    int head_dx = (neck * 2 * isin((int)a->anim * 3)) / (256 * 256);
    int abd = level(a, G_ABDOMEN, 60);
    int abd_dx = (abd * 3 * isin((int)a->anim * 4)) / (256 * 256);
    int flight = level(a, G_FLIGHT, 60);

    /* legs first so the body overlaps their roots */
    leg(a, cx - 9, cy - 17, -1, 44, level(a, G_LEG_FL, 40), 0);
    leg(a, cx + 9, cy - 17, 1, 44, level(a, G_LEG_FR, 40), 32);
    leg(a, cx - 10, cy - 10, -1, 16, level(a, G_LEG_ML, 40), 32);
    leg(a, cx + 10, cy - 10, 1, 16, level(a, G_LEG_MR, 40), 0);
    leg(a, cx - 9, cy - 3, -1, 7, level(a, G_LEG_HL, 40), 0);
    leg(a, cx + 9, cy - 3, 1, 7, level(a, G_LEG_HR, 40), 32);

    /* abdomen with tergite stripes */
    gfx_ellipse(g, cx + abd_dx, cy + 15, 9, 16 - (abd * 3) / 256, C_ABDOMEN);
    for (int k = 0; k < 4; k++) gfx_hline(g, cx + abd_dx - 7 + k / 2, cx + abd_dx + 7 - k / 2, cy + 9 + k * 6, C_STRIPE);

    /* halteres */
    int hal = level(a, G_HALTERE, 60);
    int hj = (hal * isin((int)a->anim * 19)) / (256 * 128);
    limb(a, cx - 8, cy - 3, -1, 12 + hj, 7, C_LEG, NULL, NULL);
    limb(a, cx + 8, cy - 3, 1, 12 + hj, 7, C_LEG, NULL, NULL);

    gfx_ellipse(g, cx, cy - 10, 11, 12, C_THORAX);
    wing(a, cx - 6, cy - 15, -1, level(a, G_WING_L, 60), flight);
    wing(a, cx + 6, cy - 15, 1, level(a, G_WING_R, 60), flight);

    /* head, eyes, antennae, proboscis */
    int hx = cx + head_dx, hy = cy - 27;
    int ant = level(a, G_ANTENNA, 60);
    int aj = (ant * 3 * isin((int)a->anim * 13)) / (256 * 256);
    int prob = level(a, G_PROBOSCIS, 50);
    int plen = 2 + (prob * 13) / 256;
    gfx_line(g, hx, hy - 5, hx, hy - 5 - plen, C_TONGUE);
    gfx_line(g, hx + 1, hy - 5, hx + 1, hy - 5 - plen, C_TONGUE);
    if (prob > 16) gfx_ellipse(g, hx, hy - 6 - plen, 2, 1, C_TONGUE);
    limb(a, hx - 3, hy - 5, -1, 36 + aj, 5, C_LEG, NULL, NULL);
    limb(a, hx + 3, hy - 5, 1, 36 - aj, 5, C_LEG, NULL, NULL);
    gfx_ellipse(g, hx, hy, 8, 6, C_HEAD);
    gfx_ellipse(g, hx - 7, hy, 3, 5, C_EYE);
    gfx_ellipse(g, hx + 7, hy, 3, 5, C_EYE);
}
