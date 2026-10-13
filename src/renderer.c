#include "renderer.h"

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HX_DIM_PAIR 10
#define HX_CHIP_PAIR 11
#define HX_FLASH_SECONDS 1.2

static const char *const help_lines[] = {
    " Hextrix - stationary data, living color",
    "",
    "  q / Ctrl+C   quit",
    "  space        pause / resume",
    "  r            reset fluid effects",
    "  m            toggle audio reactivity",
    "  + / -        animation intensity",
    "  up / down    hex token change speed",
    "  v            visualizer: off (liquid) / wave / rain",
    "  1-9          accent color (1 = red)",
    "  left / right fluid speed",
    "  h / ?        close this help",
    "",
    " The data never moves. Only its color does.",
};
#define HELP_COUNT ((int)(sizeof(help_lines) / sizeof(help_lines[0])))

void renderer_init(Renderer *r, int cols, int rows, bool color_ok, bool dim_pair)
{
    r->state = NULL;
    r->tok = NULL;
    r->cols = 0;
    r->rows = 0;
    r->full = true;
    r->color_ok = color_ok;
    r->dim_pair = dim_pair;
    r->accent = 1;
    r->chip_pair = false;
    r->flash[0] = '\0';
    r->flash_deadline = 0.0;
    renderer_resize(r, cols, rows);
    r->full = true;
}

void renderer_free(Renderer *r)
{
    free(r->state);
    free(r->tok);
    r->state = NULL;
    r->tok = NULL;
    r->cols = 0;
    r->rows = 0;
}

bool renderer_resize(Renderer *r, int cols, int rows)
{
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;
    if (r->state != NULL && cols == r->cols && rows == r->rows)
        return true;

    size_t n = (size_t)cols * (size_t)rows;
    unsigned char *ns = malloc(n);
    unsigned char *nt = malloc(n * 2u);
    if (ns == NULL || nt == NULL) {
        free(ns);
        free(nt);
        return false;
    }
    memset(ns, 0xff, n);     /* force redraw */
    memset(nt, 0xff, n * 2u);

    free(r->state);
    free(r->tok);
    r->state = ns;
    r->tok = nt;
    r->cols = cols;
    r->rows = rows;
    r->full = true;
    return true;
}

void renderer_invalidate(Renderer *r)
{
    r->full = true;
}

void renderer_draw(Renderer *r, const Grid *g, const Fluid *f)
{
    int cols = g->cols < r->cols ? g->cols : r->cols;
    int rows = g->rows < r->rows ? g->rows : r->rows;
    if (f->state == NULL || g->tokens == NULL || r->state == NULL ||
        r->tok == NULL)
        return;

    if (r->full)
        erase();

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            size_t ig = (size_t)row * (size_t)g->cols + (size_t)col;
            size_t ir = (size_t)row * (size_t)r->cols + (size_t)col;
            size_t ifl = (size_t)row * (size_t)f->cols + (size_t)col;
            unsigned char st = f->state[ifl];
            const unsigned char *tk = g->tokens + ig * 2u;
            bool same = !r->full && r->state[ir] == st &&
                        r->tok[ir * 2u] == tk[0] && r->tok[ir * 2u + 1u] == tk[1];
            if (same)
                continue;
            r->state[ir] = st;
            r->tok[ir * 2u] = tk[0];
            r->tok[ir * 2u + 1u] = tk[1];

            /* two foreground states: deep gray body text, bold accent */
            chtype attr = 0;
            if (st) {
                if (r->color_ok)
                    attr = (chtype)(COLOR_PAIR(r->accent) | A_BOLD);
            } else {
                attr = r->dim_pair ? (chtype)(COLOR_PAIR(HX_DIM_PAIR) | A_DIM)
                                   : (chtype)A_DIM;
            }
            int x = col * GRID_CELL_W;
            if (x + 1 < COLS)
                mvaddch(row, x + 1, (chtype)tk[1] | attr);
            if (x < COLS)
                mvaddch(row, x, (chtype)tk[0] | attr);
            if (r->full && x + 2 < COLS)
                mvaddch(row, x + 2, (chtype)' ' | A_DIM);
        }
    }
    r->full = false;
}

void renderer_flash(Renderer *r, const char *text, double now)
{
    snprintf(r->flash, sizeof r->flash, "%s", text);
    r->flash_deadline = now + HX_FLASH_SECONDS;
}

void renderer_overlay(Renderer *r, bool show, double now)
{
    if (r->flash[0] != '\0' && now >= r->flash_deadline) {
        r->flash[0] = '\0';
        r->full = true;              /* repaint the cells the label covered */
    }
    if (r->flash[0] != '\0') {
        char buf[64];
        int n = snprintf(buf, sizeof buf, "[ %s ]", r->flash);
        if (n > 0) {
            if (n >= (int)sizeof buf)
                n = (int)sizeof buf - 1;
            int x = (COLS - (n + 4)) / 2;      /* padding on both sides */
            int y = LINES / 2;
            if (x >= 0 && y >= 0 && y < LINES) {
                /* dim plate so the label reads cleanly over the tokens */
                chtype plate = r->chip_pair ? (chtype)COLOR_PAIR(HX_CHIP_PAIR)
                                            : (chtype)A_DIM;
                for (int i = 0; i < n + 4 && x + i < COLS; i++)
                    mvaddch(y, x + i, (chtype)' ' | plate);
                attrset(plate | A_BOLD);
                mvaddnstr(y, x + 2, buf, n);
                attrset(A_NORMAL);
            }
        }
    }

    if (!show)
        return;

    int w = 0;
    for (int i = 0; i < HELP_COUNT; i++) {
        int len = (int)strlen(help_lines[i]);
        if (len > w)
            w = len;
    }
    w += 4;
    int h = HELP_COUNT + 2;
    if (w > COLS)
        w = COLS;
    if (h > LINES)
        h = LINES;
    if (w < 6 || h < 4)
        return;

    int y0 = (LINES - h) / 2;
    int x0 = (COLS - w) / 2;

    for (int y = y0; y < y0 + h; y++)
        mvhline(y, x0, (chtype)' ', w);

    mvaddch(y0, x0, ACS_ULCORNER);
    mvhline(y0, x0 + 1, ACS_HLINE, w - 2);
    mvaddch(y0, x0 + w - 1, ACS_URCORNER);
    mvvline(y0 + 1, x0, ACS_VLINE, h - 2);
    mvvline(y0 + 1, x0 + w - 1, ACS_VLINE, h - 2);
    mvaddch(y0 + h - 1, x0, ACS_LLCORNER);
    mvhline(y0 + h - 1, x0 + 1, ACS_HLINE, w - 2);
    mvaddch(y0 + h - 1, x0 + w - 1, ACS_LRCORNER);

    int lines = HELP_COUNT < h - 2 ? HELP_COUNT : h - 2;
    for (int i = 0; i < lines; i++)
        mvaddnstr(y0 + 1 + i, x0 + 2, help_lines[i], w - 4);
}
