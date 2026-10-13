#ifndef HEXTRIX_FLUID_H
#define HEXTRIX_FLUID_H

#include <stdbool.h>

/*
 * Modes: 0 = liquid (default ambient organic field, full curl flow),
 * 1 = wave (cava-style bottom swell), 2 = rain (falling streaks).
 */
#define FLUID_STYLE_COUNT 3

typedef struct {
    int cols, rows;
    float aspect;            /* visual width/height of one cell */
    float time;
    float gain;              /* user intensity multiplier */
    int style;               /* 0 liquid, 1 wave, 2 rain */

    float *psi;              /* velocity potential */
    float *dye;              /* injected energy, advected each step */
    float *pvx, *pvy;        /* perturbation velocity (swirls), row-units/s */
    float *amb;              /* ambient noise value per cell */
    unsigned char *state;    /* thresholded color, 0 or 1 */

    /* smoothed audio energies, 0..1 */
    float a_bass, a_mid, a_treble, a_rms;

} Fluid;

const char *fluid_style_name(int style);

void fluid_init(Fluid *f, int cols, int rows);
void fluid_free(Fluid *f);
bool fluid_resize(Fluid *f, int cols, int rows);
void fluid_reset(Fluid *f);

/* Audio targets (already normalized 0..1); smoothed with dt. */
void fluid_audio(Fluid *f, float bass, float mid, float treble, float rms, float dt);

/* Advance the field by dt seconds. */
void fluid_step(Fluid *f, float dt);

#endif
