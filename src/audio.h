#ifndef HEXTRIX_AUDIO_H
#define HEXTRIX_AUDIO_H

#include <stdbool.h>

typedef enum {
    AUDIO_UNAVAILABLE = 0, /* not built in, or initialization failed */
    AUDIO_CONNECTING,       /* running, waiting for an audio sink */
    AUDIO_ACTIVE,           /* capturing system output */
    AUDIO_ERROR             /* stream failed */
} AudioStatus;

bool audio_init(void);
void audio_shutdown(void);
void audio_set_enabled(bool on);
bool audio_get_enabled(void);
AudioStatus audio_status(void);
const char *audio_backend(void);

/* Smoothed band energies in the range 0..1. */
void audio_read(float *bass, float *mid, float *treble, float *rms);

#endif
