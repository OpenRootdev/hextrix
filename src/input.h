#ifndef HEXTRIX_INPUT_H
#define HEXTRIX_INPUT_H

#include <stdbool.h>

/*
 * UI state machine: keyboard and pointer events are translated into a
 * small set of flags that the main loop consumes.
 */
typedef struct {
    bool quit;
    bool paused;
    bool overlay;
    bool audio_on;
    bool want_reset;
    bool want_audio_toggle;
    bool need_full_redraw;
    float gain;
    float token_rate;      /* expected token changes per cell per second */
    int rate_level;        /* index into the discrete rate ladder (0 = frozen) */
    int vis_style;         /* visualizer style, cycles with 'v' */
    int accent;            /* accent color 1..9, keys 1-9 (1 = red) */
    int speed_level;       /* index into the fluid-speed ladder */
    float speed;           /* fluid time scale, 1.0 = normal */
    double overlay_deadline;
} UiState;

void ui_init(UiState *ui);
bool ui_key(UiState *ui, int ch, double now);
bool ui_tick(UiState *ui, double now);

#endif
