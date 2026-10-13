#include "fluid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tuning constants                                                    */
/* ------------------------------------------------------------------ */

#define NOISE_K      0.055f  /* ambient noise frequency, per row       */
#define WARP_K       0.110f  /* domain warp frequency, per row         */
#define PSI_K        0.045f  /* velocity potential frequency           */
#define TIME_SCALE   0.12f   /* noise time evolution speed             */
#define WARP_AMP     1.15f   /* warp amplitude, ambient noise units    */
#define VEL_SCALE    180.0f  /* curl gain, row-units/s per unit grad   */

#define AMB_OCTAVES  3
#define WARP_OCTAVES 2
#define PSI_OCTAVES  2

#define AMB_W        0.72f   /* ambient contribution to intensity      */
#define DYE_W        0.62f   /* dye contribution to intensity          */
#define THR_HI       0.45f   /* turn red on above this                 */
#define THR_LO       0.42f   /* turn red off below this                */

#define DYE_MAX      1.5f
#define DYE_HALF_LIFE 0.80f  /* seconds                               */
#define PV_HALF_LIFE  0.70f  /* seconds                               */
#define CELL_ASPECT   1.65f  /* visual width/height of one token cell */

#define AUDIO_BASS_GAIN 1.5f
#define AUDIO_TRE_GAIN  2.5f

/* ------------------------------------------------------------------ */
/* Hash based value noise                                              */
/* ------------------------------------------------------------------ */

static float phash(int x, int y, int z)
{
    unsigned int h = (unsigned int)x * 0x8da6b343u;
    h ^= (unsigned int)y * 0xd8163841u;
    h ^= (unsigned int)z * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return (float)(h & 0x00ffffffu) * (1.0f / 16777215.0f);
}

static float fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float vnoise(float x, float y, float z)
{
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int z0 = (int)floorf(z);
    float fx = fade(x - (float)x0);
    float fy = fade(y - (float)y0);
    float fz = fade(z - (float)z0);

    float c000 = phash(x0, y0, z0);
    float c100 = phash(x0 + 1, y0, z0);
    float c010 = phash(x0, y0 + 1, z0);
    float c110 = phash(x0 + 1, y0 + 1, z0);
    float c001 = phash(x0, y0, z0 + 1);
    float c101 = phash(x0 + 1, y0, z0 + 1);
    float c011 = phash(x0, y0 + 1, z0 + 1);
    float c111 = phash(x0 + 1, y0 + 1, z0 + 1);

    float x00 = c000 + (c100 - c000) * fx;
    float x10 = c010 + (c110 - c010) * fx;
    float x01 = c001 + (c101 - c001) * fx;
    float x11 = c011 + (c111 - c011) * fx;
    float y0v = x00 + (x10 - x00) * fy;
    float y1v = x01 + (x11 - x01) * fy;
    return y0v + (y1v - y0v) * fz;
}

static float fbm(float x, float y, float z, int octaves)
{
    float amp = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    float f = 1.0f;

    for (int i = 0; i < octaves; i++) {
        sum += amp * vnoise(x * f, y * f, z * f);
        norm += amp;
        amp *= 0.5f;
        f *= 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.5f;
}

/* ------------------------------------------------------------------ */
/* Allocation helpers                                                  */
/* ------------------------------------------------------------------ */

static float *alloc_field(size_t n)
{
    float *p = malloc(n * sizeof(float));
    if (p != NULL)
        memset(p, 0, n * sizeof(float));
    return p;
}

static bool alloc_all(Fluid *f)
{
    size_t n = (size_t)f->cols * (size_t)f->rows;
    f->psi = alloc_field(n);
    f->dye = alloc_field(n);
    f->pvx = alloc_field(n);
    f->pvy = alloc_field(n);
    f->amb = alloc_field(n);
    f->state = calloc(n, 1);
    if (f->psi == NULL || f->dye == NULL || f->pvx == NULL ||
        f->pvy == NULL || f->amb == NULL || f->state == NULL) {
        free(f->psi); free(f->dye); free(f->pvx);
        free(f->pvy); free(f->amb); free(f->state);
        f->psi = f->dye = f->pvx = f->pvy = f->amb = NULL;
        f->state = NULL;
        return false;
    }
    return true;
}

void fluid_init(Fluid *f, int cols, int rows)
{
    memset(f, 0, sizeof(*f));
    f->cols = cols > 0 ? cols : 1;
    f->rows = rows > 0 ? rows : 1;
    f->aspect = CELL_ASPECT;
    f->gain = 1.0f;
    f->style = 0;
    f->time = 40.0f;
    alloc_all(f);
}

const char *fluid_style_name(int style)
{
    switch (style) {
    case 1:  return "wave";
    case 2:  return "rain";
    default: return "liquid";
    }
}

void fluid_free(Fluid *f)
{
    free(f->psi);
    free(f->dye);
    free(f->pvx);
    free(f->pvy);
    free(f->amb);
    free(f->state);
    f->psi = f->dye = f->pvx = f->pvy = f->amb = NULL;
    f->state = NULL;
}

bool fluid_resize(Fluid *f, int cols, int rows)
{
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;
    if (cols == f->cols && rows == f->rows && f->dye != NULL)
        return true;

    int old_cols = f->cols;
    int old_rows = f->rows;
    float *o_dye = f->dye;
    float *o_pvx = f->pvx;
    float *o_pvy = f->pvy;
    float *o_amb = f->amb;
    unsigned char *o_state = f->state;
    free(f->psi);
    f->psi = NULL;
    f->dye = f->pvx = f->pvy = f->amb = NULL;
    f->state = NULL;
    f->cols = cols;
    f->rows = rows;
    if (!alloc_all(f)) {
        f->cols = old_cols;
        f->rows = old_rows;
        f->dye = o_dye;
        f->pvx = o_pvx;
        f->pvy = o_pvy;
        f->amb = o_amb;
        f->state = o_state;
        return false;
    }

    int cc = cols < old_cols ? cols : old_cols;
    int cr = rows < old_rows ? rows : old_rows;
    if (o_dye != NULL) {
        for (int r = 0; r < cr; r++) {
            memcpy(f->dye + (size_t)r * cols, o_dye + (size_t)r * old_cols,
                   (size_t)cc * sizeof(float));
            memcpy(f->pvx + (size_t)r * cols, o_pvx + (size_t)r * old_cols,
                   (size_t)cc * sizeof(float));
            memcpy(f->pvy + (size_t)r * cols, o_pvy + (size_t)r * old_cols,
                   (size_t)cc * sizeof(float));
            memcpy(f->amb + (size_t)r * cols, o_amb + (size_t)r * old_cols,
                   (size_t)cc * sizeof(float));
            memcpy(f->state + (size_t)r * cols, o_state + (size_t)r * old_cols,
                   (size_t)cc);
        }
    }
    free(o_dye);
    free(o_pvx);
    free(o_pvy);
    free(o_amb);
    free(o_state);
    return true;
}

void fluid_reset(Fluid *f)
{
    size_t n = (size_t)f->cols * (size_t)f->rows;
    if (f->dye == NULL)
        return;
    memset(f->dye, 0, n * sizeof(float));
    memset(f->pvx, 0, n * sizeof(float));
    memset(f->pvy, 0, n * sizeof(float));
    memset(f->state, 0, n);
    f->time = 40.0f + (float)(rand() % 100000) / 37.0f;
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

void fluid_audio(Fluid *f, float bass, float mid, float treble, float rms, float dt)
{
    if (dt <= 0.0f)
        dt = 1.0f / 60.0f;
    if (dt > 0.1f)
        dt = 0.1f;
    float a = 1.0f - expf(-dt * 8.0f);
    f->a_bass += (bass - f->a_bass) * a;
    f->a_mid += (mid - f->a_mid) * a;
    f->a_treble += (treble - f->a_treble) * a;
    f->a_rms += (rms - f->a_rms) * a;
}

/* ------------------------------------------------------------------ */
/* Simulation                                                          */
/* ------------------------------------------------------------------ */

static float sample_dye(const Fluid *f, float x, float y)
{
    int W = f->cols;
    int H = f->rows;

    if (x < 0.0f) x = 0.0f; else if (x > (float)(W - 1)) x = (float)(W - 1);
    if (y < 0.0f) y = 0.0f; else if (y > (float)(H - 1)) y = (float)(H - 1);

    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = x0 + 1 < W ? x0 + 1 : x0;
    int y1 = y0 + 1 < H ? y0 + 1 : y0;
    float fx = x - (float)x0;
    float fy = y - (float)y0;

    float a = f->dye[(size_t)y0 * W + x0];
    float b = f->dye[(size_t)y0 * W + x1];
    float c = f->dye[(size_t)y1 * W + x0];
    float d = f->dye[(size_t)y1 * W + x1];

    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

void fluid_step(Fluid *f, float dt)
{
    if (f->dye == NULL || f->psi == NULL)
        return;
    if (dt <= 0.0f)
        return;
    if (dt > 0.1f)
        dt = 0.1f;

    const int W = f->cols;
    const int H = f->rows;
    const float aspect = f->aspect;
    const float aspect_inv = 1.0f / aspect;
    const size_t n = (size_t)W * (size_t)H;

    f->time += dt;

    /* decay the perturbation velocity */
    float pv_dec = powf(0.5f, dt / PV_HALF_LIFE);
    for (size_t i = 0; i < n; i++) {
        f->pvx[i] *= pv_dec;
        f->pvy[i] *= pv_dec;
    }

    /* velocity potential (curl source) */
    float pz = f->time * TIME_SCALE * 0.6f;
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) {
            f->psi[(size_t)r * W + c] =
                fbm((float)c * aspect * PSI_K, (float)r * PSI_K, pz, PSI_OCTAVES);
        }
    }

    const float t = f->time * TIME_SCALE;
    const float bass = f->a_bass;
    const float mid = f->a_mid;
    const float tre = f->a_treble;
    const float energy = bass > mid ? bass : mid;
    const float boost = 1.0f + mid * 2.2f;
    const float warp_amp = WARP_AMP * boost;
    const float gain = f->gain;
    const float tint = (1.0f - gain) * 0.05f;   /* intensity shifts the red area */
    float speed_gain = 1.0f + (gain - 1.0f) * 0.4f;
    if (speed_gain < 0.5f)
        speed_gain = 0.5f;
    if (speed_gain > 3.0f)
        speed_gain = 3.0f;
    float vscale = VEL_SCALE * speed_gain * (1.0f + mid * 1.5f + bass * 0.6f);
    float vy_extra = 0.0f;
    if (f->style == 1) {
        /* wave: the crest is the structure — almost no smearing */
        vscale *= 0.08f;
    } else if (f->style == 2) {
        /* rain: streaks stay vertical, fall via the added drift */
        vscale *= 0.04f;
        vy_extra = 9.0f + mid * 36.0f + bass * 18.0f;
    }
    float gs = 0.4f + 0.6f * gain;       /* intensity scales dye + wave height */
    const float thr_hi = THR_HI - energy * 0.05f + tint;
    const float thr_lo = THR_LO - energy * 0.05f + tint;
    float dye_half = DYE_HALF_LIFE;
    if (f->style == 2)
        dye_half = 1.1f;                  /* rain: clear columns quickly */
    const float dye_dec = powf(0.5f, dt / dye_half);
    const float amb_z = t * 0.9f;
    const int style = f->style;

    for (int r = 0; r < H; r++) {
        int rm = r > 0 ? r - 1 : r;
        int rp = r < H - 1 ? r + 1 : r;
        float row_scale = (r == 0 || r == H - 1) ? 1.0f : 0.5f;

        for (int c = 0; c < W; c++) {
            size_t i = (size_t)r * (size_t)W + (size_t)c;
            float X = (float)c * aspect;
            float qx = X * NOISE_K;
            float qy = (float)r * NOISE_K;

            /* domain warp */
            float w1 = fbm(X * WARP_K + 13.7f, (float)r * WARP_K + 7.1f, t, WARP_OCTAVES);
            float w2 = fbm(X * WARP_K + 31.4f, (float)r * WARP_K + 17.9f, t, WARP_OCTAVES);
            float ax = qx + (w1 - 0.5f) * warp_amp;
            float ay = qy + (w2 - 0.5f) * warp_amp;

            float amb = fbm(ax, ay, amb_z, AMB_OCTAVES);
            f->amb[i] = amb;

            /* curl of the potential -> divergence free flow */
            int cm = c > 0 ? c - 1 : c;
            int cp = c < W - 1 ? c + 1 : c;
            float col_scale = (c == 0 || c == W - 1) ? 1.0f : 0.5f;
            float dpsi_dy = (f->psi[(size_t)rp * W + c] - f->psi[(size_t)rm * W + c]) * row_scale;
            float dpsi_dx = (f->psi[(size_t)r * W + cp] - f->psi[(size_t)r * W + cm]) * col_scale;
            float vx = dpsi_dy * vscale + f->pvx[i];            /* row-units/s */
            float vy = -dpsi_dx * vscale * aspect_inv + f->pvy[i] + vy_extra;

            /* semi-Lagrangian advection of the dye */
            float sx = (float)c - vx * aspect_inv * dt;
            float sy = (float)r - vy * dt;
            float d = sample_dye(f, sx, sy) * dye_dec;

            /* per-mode injection gates: how energy paints this cell */
            float ib = 0.0f, it = 0.0f;
            if (style == 2) {
                /* rain: column streaks born at the top, carried down */
                float colg = vnoise((float)c * 2.3f + 41.0f,
                                    (float)r * 0.12f, f->time * 1.1f);
                float topw = r < 2 ? 1.0f : (r < H / 5 ? 0.4f : 0.0f);
                float mb = topw * (colg * 4.4f - 3.0f);
                if (mb > 0.0f)
                    ib = 0.50f * AUDIO_BASS_GAIN * mb;
                float nh = vnoise(qx * 4.0f + 21.1f, qy * 4.0f + 3.3f,
                                  f->time * 7.0f);
                float mt = nh * 3.4f - 2.4f;
                if (mt > 0.0f)
                    it = (0.05f + tre * 1.2f) * AUDIO_TRE_GAIN * mt;
            } else if (style == 1) {
                /* wave: smooth cava-style swell rolling along the bottom.
                   The crest line rises with the music; everything under it
                   is red, fading softly at the surface. */
                float wenergy = bass * 0.7f + mid * 0.3f;
                float level = (1.2f + (float)H * (0.05f + 0.42f * wenergy))
                              * gs;
                float crest = level * (0.72f +
                                       0.18f * sinf((float)c * 0.33f +
                                                    f->time * 1.1f) +
                                       0.22f * sinf((float)c * 0.13f -
                                                    f->time * 0.6f));
                float dist = (float)(H - 1 - r);      /* rows above bottom */
                float edge = crest - dist;
                if (edge > 0.0f) {
                    float m = edge > 1.5f ? 1.0f : edge * 0.666f;
                    ib = (0.55f + bass * 1.3f) * AUDIO_BASS_GAIN * m;
                }
                if (tre > 0.004f && edge > -2.5f && edge < 2.5f) {
                    float nh = vnoise((float)c * 3.0f + 21.1f,
                                      (float)r * 3.0f + 3.3f,
                                      f->time * 6.0f);
                    float mt = nh * 3.4f - 2.2f;
                    if (mt > 0.0f)
                        it = tre * AUDIO_TRE_GAIN * mt * 1.5f;
                }
            } else {
                /* liquid (default): organic noise-gated blobs drifting
                   with the curl flow; a small idle floor keeps it alive
                   in silence, bass scales it into full waves. */
                float nl = vnoise(qx * 0.45f + 5.3f, qy * 0.45f + 9.7f,
                                  t * 0.8f);
                float m = nl * 3.3f - 1.95f;
                if (m > 0.0f)
                    ib = (0.14f + bass * 0.85f) * AUDIO_BASS_GAIN * m;
                if (tre > 0.004f) {
                    float nh = vnoise(qx * 4.0f + 21.1f, qy * 4.0f + 3.3f,
                                      f->time * 7.0f);
                    float mt = nh * 3.4f - 2.2f;
                    if (mt > 0.0f)
                        it = tre * AUDIO_TRE_GAIN * mt;
                }
            }
            d += (ib + it) * gs * dt;
            if (d > DYE_MAX)
                d = DYE_MAX;
            f->dye[i] = d;

            /* threshold with hysteresis into the two color states */
            float v = amb * AMB_W + (d > 1.2f ? 1.2f : d) * DYE_W;
            unsigned char ns = f->state[i];
            ns = ns ? (v > thr_lo) : (v > thr_hi);
            f->state[i] = ns;
        }
    }
}
