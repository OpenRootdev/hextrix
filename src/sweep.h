#ifndef HEXTRIX_SWEEP_H
#define HEXTRIX_SWEEP_H

#include <signal.h>
#include <stdbool.h>

#include "grid.h"

/*
 * One-shot sweep mode: instantly light a random (incomplete) subset of the
 * token grid, then wipe it from top to bottom with an accelerating motion
 * over `seconds`, and return when finished.
 *
 * The terminal must already be initialized (colors, grid). Returns 0 on
 * completion or clean abort (q / Esc / Ctrl+C / signal), 1 on failure.
 */
int sweep_run(float seconds, const Grid *g, int accent, bool color_ok,
              bool dim_pair, const volatile sig_atomic_t *quit);

#endif
