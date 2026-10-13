/*
 * Hextrix — one-shot sweep mode (hextrix sweep <duration>).
 *
 * The concept is something washing down over the grid:
 *
 *   - the grid starts with a random, incomplete fill: some accent tokens,
 *     some dim tokens, and some cells with no token at all — painted the
 *     moment the command runs;
 *   - the wash front is a smooth wave rolling down from above (two sine
 *     components with a slow horizontal drift), never a straight line;
 *   - the wave is already moving on the first frames and keeps
 *     accelerating (nonzero initial velocity, then speeds up);
 *   - a glinting edge rides the wave, with short droplet trails of
 *     leftover accent cells hanging just above it;
 *   - behind the wave the cells are truly empty (no token at all);
 *   - the mode exits as soon as the wave has passed the bottom row.
 */

#include <math.h>
#include <ncurses.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "grid.h"
#include "sweep.h"

#define SWEEP_FPS_MS 16      /* ~60 frames per second */
#define SWEEP_BLANK  0.15    /* initial cells with no token at all */
#define SWEEP_LIT    0.45    /* initial cells with an accent token */
#define SWEEP_EDGE   0.70    /* glint on each column's wash front */
#define SWEEP_DROP   0.10    /* droplet residue above the wave */
#define SWEEP_TRAIL  4       /* max droplet trail length, rows */
#define SWEEP_V0     0.35    /* initial velocity: the wave moves at once */
#define SWEEP_WAVES  1.5     /* wave crests across the terminal width */
#define SWEEP_DRIFT  1.4     /* horizontal drift speed, rad/s */

/* initial cell state */
#define ST_DIM   0
#define ST_LIT   1
#define ST_BLANK 2

/* mirror of HX_DIM_PAIR in renderer.c: deep gray body text */
#define SWEEP_DIM_PAIR 10

static double sweep_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int sweep_run(float seconds, const Grid *g, bool color_ok, bool dim_pair,
              const volatile sig_atomic_t *quit)
{
    if (g->cols < 1 || g->rows < 1 || g->tokens == NULL)
        return 1;

    size_t n = (size_t)g->cols * (size_t)g->rows;
    unsigned char *masks = malloc(n * 3u);
    int *trail = malloc((size_t)g->cols * sizeof *trail);
    if (masks == NULL || trail == NULL) {
        free(masks);
        free(trail);
        return 1;
    }
    unsigned char *init = masks;           /* ST_DIM / ST_LIT / ST_BLANK */
    unsigned char *edge = masks + n;       /* front glint mask */
    unsigned char *drop = masks + 2u * n;  /* droplet residue mask */

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (size_t i = 0; i < n; i++) {
        double r = (double)rand() / (double)RAND_MAX;
        init[i] = r < SWEEP_BLANK ? ST_BLANK
                : r < SWEEP_BLANK + SWEEP_LIT ? ST_LIT
                : ST_DIM;
        edge[i] = ((double)rand() / (double)RAND_MAX) < SWEEP_EDGE;
        drop[i] = ((double)rand() / (double)RAND_MAX) < SWEEP_DROP;
    }
    for (int c = 0; c < g->cols; c++)
        trail[c] = 1 + (rand() % SWEEP_TRAIL);

    /* the wave travels over the whole grid plus room for its amplitude */
    const double amp = (double)g->rows / 8.0 + 1.0;   /* sine amplitude */
    const double wmax = amp * 1.4;                    /* wave[] bound */
    const double span = (double)g->rows + 2.0 * wmax;
    const double k1 = 6.283185307179586 * SWEEP_WAVES / (double)g->cols;
    const double k2 = 6.283185307179586 * (SWEEP_WAVES * 2.0 + 0.5) /
                      (double)g->cols;

    chtype acc = color_ok ? (chtype)(COLOR_PAIR(1) | A_BOLD) : (chtype)A_BOLD;
    chtype body = dim_pair ? (chtype)(COLOR_PAIR(SWEEP_DIM_PAIR) | A_DIM)
                           : (chtype)A_DIM;

    const double dur = (double)seconds;
    const double t0 = sweep_now();

    while (1) {
        double t = sweep_now() - t0;
        double u = t / dur;
        int done = 0;
        if (u >= 1.0) {
            u = 1.0;
            done = 1;
        }

        /* starts moving at once (v0 > 0) and keeps accelerating */
        double pos = SWEEP_V0 * u + (1.0 - SWEEP_V0) * u * u;
        double base = done ? (double)(g->rows + 1) : pos * span - wmax;
        double p1 = t * SWEEP_DRIFT;
        double p2 = -t * SWEEP_DRIFT * 1.6 + 1.7;

        erase();
        for (int col = 0; col < g->cols; col++) {
            /* smooth wavy front rolling down from above */
            double wave = amp * (sin(k1 * (double)col + p1)
                                 + 0.4 * sin(k2 * (double)col + p2));
            int lc = done ? g->rows : (int)(base + wave);

            for (int row = 0; row < g->rows; row++) {
                size_t idx = (size_t)row * (size_t)g->cols + (size_t)col;
                int x = col * GRID_CELL_W;
                const unsigned char *tk = g->tokens + idx * 2u;
                int state = -1;                      /* -1: leave empty */

                if (row < lc) {
                    /* washed: empty, with short droplet trails at the front */
                    if (!done && row + trail[col] >= lc && drop[idx])
                        state = ST_LIT;
                } else if (row == lc && !done && edge[idx]) {
                    state = ST_LIT;                  /* glint on the front */
                } else {
                    state = init[idx];
                }

                if (state == ST_BLANK || state < 0)
                    continue;                        /* truly empty cell */
                chtype a = state == ST_LIT ? acc : body;
                if (x + 1 < COLS)
                    mvaddch(row, x + 1, (chtype)tk[1] | a);
                if (x < COLS)
                    mvaddch(row, x, (chtype)tk[0] | a);
                if (x + 2 < COLS)
                    mvaddch(row, x + 2, (chtype)' ' | A_DIM);
            }
        }
        refresh();

        if (done)
            break;

        wtimeout(stdscr, SWEEP_FPS_MS);
        int ch = getch();
        while (ch != ERR) {
            if (ch == 'q' || ch == 27 || ch == 3)
                goto out;                            /* clean abort */
            ch = getch();
        }
        if (quit != NULL && *quit)
            goto out;
    }

out:
    free(trail);
    free(masks);
    return 0;
}
