/*
 * audio.c — system output capture for Hextrix.
 *
 * When built with PipeWire (pkg-config: libpipewire-0.3) a capture stream
 * is opened against the highest priority Audio/Sink node, using the sink
 * monitor (PW_KEY_STREAM_CAPTURE_SINK), and the incoming signal is reduced
 * to three smoothed band energies (bass / mid / treble) plus overall RMS.
 *
 * Everything runs on a PipeWire thread loop; the render thread only ever
 * touches atomic scalars. If PipeWire is missing, unreachable, or has no
 * sink, the module degrades to silence and the animation stays ambient.
 */

#include "audio.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef HEXTRIX_HAVE_PIPEWIRE

bool audio_init(void) { return false; }
void audio_shutdown(void) { }
void audio_set_enabled(bool on) { (void)on; }
bool audio_get_enabled(void) { return false; }
AudioStatus audio_status(void) { return AUDIO_UNAVAILABLE; }
const char *audio_backend(void) { return "none (built without PipeWire)"; }
void audio_read(float *bass, float *mid, float *treble, float *rms)
{
    *bass = 0.0f;
    *mid = 0.0f;
    *treble = 0.0f;
    *rms = 0.0f;
}

#else /* HEXTRIX_HAVE_PIPEWIRE */

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>

#define AUDIO_RATE_DEFAULT 48000u
#define AUDIO_MAP_GAIN 10.0f
#define AUDIO_EMA 0.25f

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} Biquad;

typedef struct {
    struct pw_thread_loop *loop;
    struct pw_context *ctx;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_hook;
    bool registry_hooked;
    bool loop_started;

    struct pw_stream *stream;
    struct spa_hook stream_hook;

    uint32_t target_id;
    int target_prio;
    bool have_target;
    bool want_stream;

    uint32_t rate;
    uint32_t channels;
    Biquad f_lo, f_mid, f_hi;

    atomic_int status;
    atomic_bool enabled;
    _Atomic float out_bass, out_mid, out_treble, out_rms;
} AudioCtx;

static AudioCtx A;

/* ---------------- biquad filters (RBJ cookbook) ---------------- */

static void bq_reset(Biquad *b)
{
    b->z1 = 0.0f;
    b->z2 = 0.0f;
}

static void bq_lp(Biquad *b, float fc, float q, float rate)
{
    float w = 2.0f * (float)M_PI * fc / rate;
    float cw = cosf(w);
    float sw = sinf(w);
    float al = sw / (2.0f * q);
    float a0 = 1.0f + al;
    b->b0 = ((1.0f - cw) * 0.5f) / a0;
    b->b1 = (1.0f - cw) / a0;
    b->b2 = b->b0;
    b->a1 = (-2.0f * cw) / a0;
    b->a2 = (1.0f - al) / a0;
    bq_reset(b);
}

static void bq_hp(Biquad *b, float fc, float q, float rate)
{
    float w = 2.0f * (float)M_PI * fc / rate;
    float cw = cosf(w);
    float sw = sinf(w);
    float al = sw / (2.0f * q);
    float a0 = 1.0f + al;
    b->b0 = ((1.0f + cw) * 0.5f) / a0;
    b->b1 = (-(1.0f + cw)) / a0;
    b->b2 = b->b0;
    b->a1 = (-2.0f * cw) / a0;
    b->a2 = (1.0f - al) / a0;
    bq_reset(b);
}

static void bq_bp(Biquad *b, float fc, float q, float rate)
{
    float w = 2.0f * (float)M_PI * fc / rate;
    float cw = cosf(w);
    float sw = sinf(w);
    float al = sw / (2.0f * q);
    float a0 = 1.0f + al;
    b->b0 = al / a0;
    b->b1 = 0.0f;
    b->b2 = -al / a0;
    b->a1 = (-2.0f * cw) / a0;
    b->a2 = (1.0f - al) / a0;
    bq_reset(b);
}

static float bq_run(Biquad *b, float x)
{
    float y = b->b0 * x + b->z1;
    b->z1 = b->b1 * x - b->a1 * y + b->z2;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}

static void design_filters(AudioCtx *a)
{
    float rate = a->rate > 0 ? (float)a->rate : (float)AUDIO_RATE_DEFAULT;
    bq_lp(&a->f_lo, 130.0f, 0.707f, rate);
    bq_bp(&a->f_mid, 900.0f, 1.1f, rate);
    bq_hp(&a->f_hi, 4200.0f, 0.707f, rate);
}

/* Map a band RMS onto 0..1 with soft saturation. */
static float map_level(float rms)
{
    float s = rms * AUDIO_MAP_GAIN;
    return s / (1.0f + s);
}

static void ema(_Atomic float *slot, float target)
{
    float cur = atomic_load_explicit(slot, memory_order_relaxed);
    cur += (target - cur) * AUDIO_EMA;
    atomic_store_explicit(slot, cur, memory_order_relaxed);
}

/* ---------------- stream handling (loop thread) ---------------- */

static void destroy_stream(AudioCtx *a)
{
    if (a->stream != NULL) {
        pw_stream_destroy(a->stream);
        a->stream = NULL;
    }
}

static void on_state_changed(void *data, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
    AudioCtx *a = data;
    (void)old;
    (void)error;

    switch (state) {
    case PW_STREAM_STATE_STREAMING:
        atomic_store(&a->status, AUDIO_ACTIVE);
        break;
    case PW_STREAM_STATE_ERROR:
        atomic_store(&a->status, AUDIO_ERROR);
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        if (atomic_load(&a->status) == AUDIO_ACTIVE)
            atomic_store(&a->status, AUDIO_CONNECTING);
        break;
    default:
        break;
    }
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    AudioCtx *a = data;
    if (id != SPA_PARAM_Format || param == NULL)
        return;

    struct spa_audio_info info;
    uint32_t media_type = 0;
    uint32_t media_subtype = 0;
    memset(&info, 0, sizeof(info));
    if (spa_format_parse(param, &media_type, &media_subtype) < 0)
        return;
    if (media_type != SPA_MEDIA_TYPE_audio || media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
        return;

    uint32_t rate = info.info.raw.rate > 0 ? info.info.raw.rate : AUDIO_RATE_DEFAULT;
    uint32_t channels = info.info.raw.channels > 0 ? info.info.raw.channels : 1;
    if (rate != a->rate || channels != a->channels) {
        a->rate = rate;
        a->channels = channels;
        design_filters(a);
    }
}

static void on_process(void *data)
{
    AudioCtx *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->stream);
    if (b == NULL)
        return;

    if (b->buffer == NULL || b->buffer->n_datas == 0) {
        pw_stream_queue_buffer(a->stream, b);
        return;
    }
    struct spa_data *d = &b->buffer->datas[0];
    uint32_t ch = a->channels > 0 ? a->channels : 1u;

    if (d->data == NULL || d->chunk == NULL || d->chunk->size == 0 ||
        ch > 64u) {
        pw_stream_queue_buffer(a->stream, b);
        return;
    }

    const float *in = (const float *)((const unsigned char *)d->data + d->chunk->offset);
    uint32_t frames = d->chunk->size / (uint32_t)sizeof(float) / ch;
    if (frames == 0) {
        pw_stream_queue_buffer(a->stream, b);
        return;
    }
    if (frames > 65536u)
        frames = 65536u;

    double s_rms = 0.0, s_lo = 0.0, s_mid = 0.0, s_hi = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        float acc = 0.0f;
        for (uint32_t c = 0; c < ch; c++)
            acc += in[i * ch + c];
        float x = acc / (float)ch;
        float lo = bq_run(&a->f_lo, x);
        float mid = bq_run(&a->f_mid, x);
        float hi = bq_run(&a->f_hi, x);
        s_rms += (double)x * x;
        s_lo += (double)lo * lo;
        s_mid += (double)mid * mid;
        s_hi += (double)hi * hi;
    }

    float inv = 1.0f / (float)frames;
    ema(&a->out_rms, map_level(sqrtf((float)s_rms * inv)));
    ema(&a->out_bass, map_level(sqrtf((float)s_lo * inv)));
    ema(&a->out_mid, map_level(sqrtf((float)s_mid * inv)));
    ema(&a->out_treble, map_level(sqrtf((float)s_hi * inv)));

    pw_stream_queue_buffer(a->stream, b);
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

static void create_stream(AudioCtx *a)
{
    if (a->stream != NULL || !a->have_target || a->loop == NULL)
        return;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_NODE_NAME, "hextrix.capture",
        PW_KEY_NODE_DESCRIPTION, "Hextrix audio analysis",
        PW_KEY_STREAM_CAPTURE_SINK, "true",
        NULL);
    if (props == NULL)
        return;

    a->stream = pw_stream_new_simple(pw_thread_loop_get_loop(a->loop),
                                     "hextrix-capture", props,
                                     &stream_events, a);
    if (a->stream == NULL)
        return; /* PipeWire takes ownership of props */

    struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = AUDIO_RATE_DEFAULT,
        .channels = 1,
        .position = { SPA_AUDIO_CHANNEL_MONO });

    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                     PW_STREAM_FLAG_RT_PROCESS;
    int res = pw_stream_connect(a->stream, PW_DIRECTION_INPUT, a->target_id,
                                flags, params, 1);
    if (res < 0) {
        pw_stream_destroy(a->stream);
        a->stream = NULL;
        atomic_store(&a->status, AUDIO_ERROR);
        return;
    }
    atomic_store(&a->status, AUDIO_CONNECTING);
}

/* ---------------- registry (loop thread) ---------------- */

static void on_global(void *data, uint32_t id, uint32_t permissions,
                      const char *type, uint32_t version,
                      const struct spa_dict *props)
{
    AudioCtx *a = data;
    (void)permissions;
    (void)version;

    if (props == NULL || type == NULL || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;
    const char *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (mc == NULL || strcmp(mc, "Audio/Sink") != 0)
        return;

    const char *pri_s = spa_dict_lookup(props, "priority.session");
    int pri = pri_s != NULL ? atoi(pri_s) : 0;
    if (a->have_target && pri <= a->target_prio)
        return;

    a->target_id = id;
    a->target_prio = pri;
    a->have_target = true;
    if (a->want_stream)
        create_stream(a);
}

static void on_global_remove(void *data, uint32_t id)
{
    AudioCtx *a = data;
    if (!a->have_target || id != a->target_id)
        return;
    destroy_stream(a);
    a->have_target = false;
    atomic_store(&a->status, AUDIO_CONNECTING);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    on_global,
    on_global_remove,
};

/* ---------------- lifecycle ---------------- */

bool audio_init(void)
{
    memset(&A, 0, sizeof(A));
    atomic_init(&A.status, AUDIO_UNAVAILABLE);
    atomic_init(&A.enabled, true);
    atomic_init(&A.out_bass, 0.0f);
    atomic_init(&A.out_mid, 0.0f);
    atomic_init(&A.out_treble, 0.0f);
    atomic_init(&A.out_rms, 0.0f);
    A.target_prio = -1;
    A.want_stream = true;
    A.rate = AUDIO_RATE_DEFAULT;
    A.channels = 1;
    design_filters(&A);

    int c = 1;
    char *dummy[] = { (char *)"hextrix", NULL };
    char **v = dummy;
    pw_init(&c, &v);

    A.loop = pw_thread_loop_new("hextrix-audio", NULL);
    if (A.loop == NULL)
        return false;

    A.ctx = pw_context_new(pw_thread_loop_get_loop(A.loop), NULL, false);
    if (A.ctx == NULL)
        goto fail;

    A.core = pw_context_connect(A.ctx, NULL, 0);
    if (A.core == NULL)
        goto fail;

    A.registry = pw_core_get_registry(A.core, PW_VERSION_REGISTRY, 0);
    if (A.registry == NULL)
        goto fail;

    pw_registry_add_listener(A.registry, &A.registry_hook, &registry_events, &A);
    A.registry_hooked = true;

    if (pw_thread_loop_start(A.loop) != 0)
        goto fail;

    A.loop_started = true;
    atomic_store(&A.status, AUDIO_CONNECTING);
    return true;

fail:
    audio_shutdown();
    return false;
}

void audio_shutdown(void)
{
    if (A.loop != NULL) {
        if (A.loop_started) {
            pw_thread_loop_lock(A.loop);
            destroy_stream(&A);
            pw_thread_loop_unlock(A.loop);
            pw_thread_loop_stop(A.loop);
            A.loop_started = false;
        }
        if (A.registry_hooked) {
            spa_hook_remove(&A.registry_hook);
            A.registry_hooked = false;
        }
        if (A.registry != NULL) {
            pw_proxy_destroy((struct pw_proxy *)A.registry);
            A.registry = NULL;
        }
        if (A.core != NULL) {
            pw_core_disconnect(A.core);
            A.core = NULL;
        }
        if (A.ctx != NULL) {
            pw_context_destroy(A.ctx);
            A.ctx = NULL;
        }
        pw_thread_loop_destroy(A.loop);
        A.loop = NULL;
        pw_deinit();
    }
    atomic_store(&A.status, AUDIO_UNAVAILABLE);
}

void audio_set_enabled(bool on)
{
    atomic_store(&A.enabled, on);
    if (A.loop == NULL)
        return;

    pw_thread_loop_lock(A.loop);
    A.want_stream = on;
    if (!on) {
        destroy_stream(&A);
    } else if (A.have_target && A.stream == NULL) {
        create_stream(&A);
    }
    pw_thread_loop_unlock(A.loop);
}

bool audio_get_enabled(void)
{
    return atomic_load(&A.enabled);
}

AudioStatus audio_status(void)
{
    return (AudioStatus)atomic_load(&A.status);
}

const char *audio_backend(void)
{
    return "pipewire";
}

void audio_read(float *bass, float *mid, float *treble, float *rms)
{
    *bass = atomic_load_explicit(&A.out_bass, memory_order_relaxed);
    *mid = atomic_load_explicit(&A.out_mid, memory_order_relaxed);
    *treble = atomic_load_explicit(&A.out_treble, memory_order_relaxed);
    *rms = atomic_load_explicit(&A.out_rms, memory_order_relaxed);
}

#endif /* HEXTRIX_HAVE_PIPEWIRE */
