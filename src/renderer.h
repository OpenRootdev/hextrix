#ifndef HEXTRIX_RENDERER_H
#define HEXTRIX_RENDERER_H

#include <stdbool.h>

#include "fluid.h"
#include "grid.h"

/*
 * Renders the grid through the fluid field.
 *
 * Only cells whose color state changed are touched, so ncurses only has
 * to diff a small number of lines on refresh.
 */
typedef struct {
    unsigned char *state;
    unsigned char *tok;   /* last rendered token bytes, 2 per cell */
    int cols;
    int rows;
    bool full;        /* next draw repaints everything */
    bool color_ok;    /* accent pairs 1..9 are usable */
    bool dim_pair;    /* pair 10 (deep gray body text) is usable */
    int accent;       /* selected accent pair, 1..9 (1 = red) */
    bool chip_pair;   /* pair 11 (dim plate behind flash labels) works */
    char flash[48];   /* transient centered label (switch hints) */
    double flash_deadline;
} Renderer;

void renderer_init(Renderer *r, int cols, int rows, bool color_ok, bool dim_pair);
void renderer_free(Renderer *r);
bool renderer_resize(Renderer *r, int cols, int rows);
void renderer_invalidate(Renderer *r);
void renderer_draw(Renderer *r, const Grid *g, const Fluid *f);
void renderer_overlay(Renderer *r, bool show, double now);
void renderer_flash(Renderer *r, const char *text, double now);

#endif
