#include "grid.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned int g_rng = 0x2545f491u;

static unsigned int rng_next(void)
{
    unsigned int x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

static void seed_rng(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        g_rng ^= (unsigned int)ts.tv_nsec;
    if (g_rng == 0)
        g_rng = 0x2545f491u;
}

static void fill_token(unsigned char *dst, GridMode mode)
{
    static const char hex[] = "0123456789ABCDEF";
    static const char alnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    if (mode == GRID_HEX) {
        dst[0] = (unsigned char)hex[rng_next() & 15u];
        dst[1] = (unsigned char)hex[rng_next() & 15u];
    } else {
        dst[0] = (unsigned char)alnum[rng_next() % 36u];
        dst[1] = (unsigned char)alnum[rng_next() % 36u];
    }
}

static int calc_cols(int term_cols)
{
    int cols = term_cols / GRID_CELL_W;
    if (cols < 1)
        cols = 1;
    return cols;
}

static int calc_rows(int term_rows)
{
    int rows = term_rows;
    if (rows < 1)
        rows = 1;
    return rows;
}

void grid_init(Grid *g, int term_cols, int term_rows, GridMode mode)
{
    seed_rng();
    g->mode = mode;
    g->tokens = NULL;
    g->cols = 0;
    g->rows = 0;
    g->term_cols = 0;
    g->term_rows = 0;
    grid_resize(g, term_cols, term_rows);
}

void grid_free(Grid *g)
{
    free(g->tokens);
    g->tokens = NULL;
    g->cols = 0;
    g->rows = 0;
}

bool grid_resize(Grid *g, int term_cols, int term_rows)
{
    int cols = calc_cols(term_cols);
    int rows = calc_rows(term_rows);
    int old_cols = g->cols;
    int old_rows = g->rows;
    unsigned char *old = g->tokens;

    if (old != NULL && cols == old_cols && rows == old_rows) {
        g->term_cols = term_cols;
        g->term_rows = term_rows;
        return true;
    }

    unsigned char *tok = malloc((size_t)cols * (size_t)rows * 2u);
    if (tok == NULL)
        return false;

    int copy_cols = cols < old_cols ? cols : old_cols;
    int copy_rows = rows < old_rows ? rows : old_rows;
    if (old != NULL) {
        for (int r = 0; r < copy_rows; r++)
            memcpy(tok + ((size_t)r * (size_t)cols) * 2u,
                   old + ((size_t)r * (size_t)old_cols) * 2u,
                   (size_t)copy_cols * 2u);
    }

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (old != NULL && r < copy_rows && c < copy_cols)
                continue;
            fill_token(tok + (((size_t)r * (size_t)cols) + (size_t)c) * 2u, g->mode);
        }
    }

    free(old);
    g->tokens = tok;
    g->cols = cols;
    g->rows = rows;
    g->term_cols = term_cols;
    g->term_rows = term_rows;
    return true;
}

void grid_mutate(Grid *g, int count)
{
    int total = g->cols * g->rows;
    if (g->tokens == NULL || count <= 0 || total <= 0)
        return;
    if (count > total)
        count = total;
    for (int i = 0; i < count; i++) {
        unsigned int idx = rng_next() % (unsigned int)total;
        fill_token(g->tokens + (size_t)idx * 2u, g->mode);
    }
}
