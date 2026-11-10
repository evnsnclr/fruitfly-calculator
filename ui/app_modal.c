/* Full-screen overlays: experiment list, cell-type picker, help. */
#include <stdio.h>
#include <string.h>

#include "app.h"

#define C_BG RGB565(8, 10, 18)
#define C_TEXT RGB565(220, 225, 235)
#define C_DIM RGB565(120, 130, 150)
#define C_TITLE RGB565(255, 220, 90)
#define C_SEL RGB565(40, 70, 130)
#define ROWS 22
#define ROW_H 9
#define LIST_Y 22

enum { EXTRA_PICK, EXTRA_SILENCE, EXTRA_NONE, EXTRA_COUNT };

static const char *const EXTRA_NAME[EXTRA_COUNT] = {
    "Pick any cell type...  (T)",
    "Silence a cell type... (X)",
    "No stimulus",
};

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* 2: text starts with query; 1: contains it; 0: no match. */
static int match_text(const app_t *a, const char *s, uint32_t len) {
    if (len < a->qlen) return 0;
    for (uint32_t i = 0; i + a->qlen <= len; i++) {
        uint32_t k = 0;
        while (k < a->qlen && lower((unsigned char)s[i + k]) == a->query[k]) k++;
        if (k == a->qlen) return i == 0 ? 2 : 1;
    }
    return 0;
}

/* Best match of the query against the type's name or literature alias. */
static int match_type(const app_t *a, uint32_t type) {
    if (a->qlen == 0) return 2;
    int best = match_text(a, a->tnam_chars + a->tnam_off[type], a->tnam_off[type + 1] - a->tnam_off[type]);
    if (best < 2 && a->tali_off) {
        int alt = match_text(a, a->tali_chars + a->tali_off[type], a->tali_off[type + 1] - a->tali_off[type]);
        if (alt > best) best = alt;
    }
    return best;
}

static void refresh_matches(app_t *a) {
    a->n_match = 0;
    a->pick_sel = 0;
    a->pick_top = 0;
    for (int want = 2; want >= 1; want--)
        for (uint32_t t = 1; t < a->n_types && a->n_match < APP_MATCH_CAP; t++)
            if (match_type(a, t) == want) a->match[a->n_match++] = t;
}

void app_open_picker(app_t *a, int silence) {
    a->mode = MODE_PICK;
    a->pick_silence = silence;
    a->qlen = 0;
    a->query[0] = 0;
    refresh_matches(a);
}

static void scroll_into_view(int sel, int *top) {
    if (sel < *top) *top = sel;
    if (sel >= *top + ROWS) *top = sel - ROWS + 1;
}

static void menu_input(app_t *a, const plat_input_t *in) {
    const int n = (int)a->n_scen + EXTRA_COUNT;
    uint32_t p = in->pressed;
    if (p & PK_DOWN) a->menu_sel = (a->menu_sel + 1) % n;
    if (p & PK_UP) a->menu_sel = (a->menu_sel + n - 1) % n;
    scroll_into_view(a->menu_sel, &a->menu_top);
    if (p & (PK_ESC | PK_MENU)) a->mode = MODE_RUN;
    if (in->typed == 't') app_open_picker(a, 0);
    if (in->typed == 'x') app_open_picker(a, 1);
    if (p & PK_ENTER) {
        int extra = a->menu_sel - (int)a->n_scen;
        a->mode = MODE_RUN;
        if (extra < 0) app_select_scenario(a, a->menu_sel);
        else if (extra == EXTRA_PICK) app_open_picker(a, 0);
        else if (extra == EXTRA_SILENCE) app_open_picker(a, 1);
        else app_select_scenario(a, SCN_NONE);
    }
}

static void pick_input(app_t *a, const plat_input_t *in) {
    uint32_t p = in->pressed;
    if (p & PK_ESC) {
        a->mode = MODE_RUN;
        return;
    }
    char c = in->typed;
    int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ' || c == '(';
    if (ok && a->qlen < APP_QUERY_CAP) {
        a->query[a->qlen++] = c;
        a->query[a->qlen] = 0;
        refresh_matches(a);
    }
    if ((p & PK_DEL) && a->qlen) {
        a->query[--a->qlen] = 0;
        refresh_matches(a);
    }
    if ((p & PK_DOWN) && a->pick_sel + 1 < (int)a->n_match) a->pick_sel++;
    if ((p & PK_UP) && a->pick_sel > 0) a->pick_sel--;
    scroll_into_view(a->pick_sel, &a->pick_top);
    if ((p & PK_ENTER) && a->n_match) {
        a->mode = MODE_RUN;
        app_pick_type(a, a->match[a->pick_sel], a->pick_silence);
    }
}

void app_modal_input(app_t *a, const plat_input_t *in) {
    switch (a->mode) {
    case MODE_MENU: menu_input(a, in); break;
    case MODE_PICK: pick_input(a, in); break;
    case MODE_HELP:
        if (in->pressed || in->typed) a->mode = MODE_RUN;
        break;
    default: break;
    }
}

static void row(app_t *a, int idx, int top, int selected, const char *text, const char *right) {
    int y = LIST_Y + (idx - top) * ROW_H;
    if (selected) gfx_rect(&a->gfx, 4, y - 1, GFX_W - 8, ROW_H, C_SEL);
    gfx_text(&a->gfx, 8, y, text, 34, C_TEXT);
    if (right) gfx_text(&a->gfx, GFX_W - 8 - (int)strlen(right) * GFX_FONT_W, y, right, 16, C_DIM);
}

static void menu_draw(app_t *a) {
    char right[24];
    gfx_text(&a->gfx, 8, 6, "Experiments", -1, C_TITLE);
    gfx_text(&a->gfx, 150, 6, "enter: run   esc: back", -1, C_DIM);
    const int n = (int)a->n_scen + EXTRA_COUNT;
    for (int i = a->menu_top; i < n && i < a->menu_top + ROWS; i++) {
        if (i < (int)a->n_scen) {
            snprintf(right, sizeof right, "%lu cells", (unsigned long)a->scen[i].n_ids);
            char name[sizeof a->scen[0].name];
            memcpy(name, a->scen[i].name, sizeof name);
            name[sizeof name - 1] = 0;
            row(a, i, a->menu_top, i == a->menu_sel, name, right);
        } else {
            row(a, i, a->menu_top, i == a->menu_sel, EXTRA_NAME[i - (int)a->n_scen], NULL);
        }
    }
    if (a->menu_sel < (int)a->n_scen) {
        gfx_text(&a->gfx, 8, 228, "watch:", -1, C_DIM);
        gfx_textn(&a->gfx, 50, 228, a->scen[a->menu_sel].hint, (int)sizeof a->scen[0].hint - 1, C_TEXT);
    }
}

static void pick_draw(app_t *a) {
    char name[40], right[24], line[48];
    gfx_text(&a->gfx, 8, 6, a->pick_silence ? "Silence type:" : "Stimulate type:", -1, C_TITLE);
    snprintf(line, sizeof line, "%s_", a->query);
    gfx_text(&a->gfx, 106, 6, line, -1, C_TEXT);
    snprintf(right, sizeof right, "%lu%s", (unsigned long)a->n_match, a->n_match == APP_MATCH_CAP ? "+" : "");
    gfx_text(&a->gfx, GFX_W - 8 - (int)strlen(right) * GFX_FONT_W, 6, right, -1, C_DIM);
    for (int i = a->pick_top; i < (int)a->n_match && i < a->pick_top + ROWS; i++) {
        uint32_t t = a->match[i];
        app_type_label(a, t, name, sizeof name);
        snprintf(right, sizeof right, "%u cells", (unsigned)a->tcnt[t]);
        row(a, i, a->pick_top, i == a->pick_sel, name, right);
    }
    gfx_text(&a->gfx, 8, 228, "type to search  enter: choose  esc: back", -1, C_DIM);
}

static void help_draw(app_t *a) {
    static const char *const LINES[] = {
        "Every neuron of the male fruit fly CNS,",
        "spiking on this calculator.",
        "",
        "up/down     previous / next experiment",
        "menu        experiment list",
        "left/right  stimulus rate",
        "enter       stimulus on / off",
        "S           stimulate both / left / right",
        "T           stimulate any cell type",
        "X           silence a cell type (toggle)",
        "+ / -       synaptic gain (try a seizure)",
        "x / :       faster / slower playback",
        "W           1-synapse connections on / off",
        "tab         fly / activity / system view",
        "P pause  R reset  A auto-tour  esc quit",
        "",
        "Map: brain (front view) above the nerve",
        "cord (top view). One dot per neuron.",
        "Data: Janelia FlyEM male CNS v1.0, CC-BY",
        "Model: Shiu et al. 2024 LIF, 1 ms steps",
    };
    gfx_text(&a->gfx, 8, 6, "Drosophila on a TI-Nspire CX II", -1, C_TITLE);
    for (unsigned i = 0; i < sizeof LINES / sizeof LINES[0]; i++) gfx_text(&a->gfx, 8, 22 + (int)i * 10, LINES[i], -1, C_TEXT);
    gfx_text(&a->gfx, 8, 228, "any key: back", -1, C_DIM);
}

void app_modal_draw(app_t *a) {
    gfx_clear(&a->gfx, C_BG);
    switch (a->mode) {
    case MODE_MENU: menu_draw(a); break;
    case MODE_PICK: pick_draw(a); break;
    case MODE_HELP: help_draw(a); break;
    default: break;
    }
}
