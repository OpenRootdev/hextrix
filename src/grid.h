#ifndef HEXTRIX_GRID_H
#define HEXTRIX_GRID_H

#include <stdbool.h>

/* Columns occupied by one token cell: two characters plus a separator space. */
#define GRID_CELL_W 3

typedef enum {
    GRID_HEX = 0,
    GRID_ALNUM = 1
} GridMode;

/*
 * A stationary grid of two-character tokens.
 *
 * The tokens are generated once and never move; only colors change.
 * Resizing keeps the overlapping region intact and only regenerates
 * newly exposed cells.
 */
typedef struct {
    int cols;                 /* token cells per row */
    int rows;                 /* token cell rows */
    int term_cols;            /* terminal columns used */
    int term_rows;            /* terminal rows used */
    GridMode mode;
    unsigned char *tokens;    /* two characters per cell, row major */
} Grid;

void grid_init(Grid *g, int term_cols, int term_rows, GridMode mode);
void grid_free(Grid *g);
bool grid_resize(Grid *g, int term_cols, int term_rows);

/* Randomly replace up to count tokens with freshly generated ones. */
void grid_mutate(Grid *g, int count);

#endif
