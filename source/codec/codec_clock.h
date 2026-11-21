#ifndef MP3PATCH_CODEC_CLOCK_H
#define MP3PATCH_CODEC_CLOCK_H
#include <math.h>
#include <stdint.h>
typedef struct {
    double host, phase;
    int hz, first, seen_edge;
    uint64_t sample, last_edge, next_adjust;
} CodecClock;
static const int codec_rates[] = {
    48000, 44100, 32000, 24000, 22050, 16000,
#ifndef MPEG_AUDIO_LAYERS_12
    12000, 11025, 8000,
#endif
};
enum { CODEC_RATE_COUNT = sizeof(codec_rates) / sizeof(codec_rates[0]) };
/* Stay in the native rate family. For example 44.1 / 2 and / 4 give
   22.05 and 11.025 kHz, while a 48 kHz clock also permits / 3 and / 6. */
static inline int codec_divide(int native, int factor) {
    int result = native;
    for (int i = 0; i < CODEC_RATE_COUNT; i++) {
        int hz = codec_rates[i];
        if (hz <= native && native % hz == 0 && native / hz <= factor)
            result = hz;
    }
    return result;
}
static inline int codec_factor(double value) {
    if (!isfinite(value) || value < 0 || value != floor(value))
        return -1;
    return value <= 1 ? 1 : value >= 6 ? 6 : (int)value;
}
static inline int codec_clock_setup(CodecClock *c, double host) {
    *c = (CodecClock){.host = host, .first = 1};
    if (!isfinite(host) || host < 1)
        return 0;
    for (int i = 0; i < CODEC_RATE_COUNT; i++) {
        double multiple = round(host / codec_rates[i]);
        if (multiple >= 1 && fabs(host - codec_rates[i] * multiple) < 1.e-7) {
            c->hz = codec_rates[i];
            return 1;
        }
    }
    for (int i = 0; i < CODEC_RATE_COUNT; i++)
        if (host >= codec_rates[i]) {
            c->hz = codec_rates[i];
            return 1;
        }
    c->hz = codec_rates[CODEC_RATE_COUNT - 1];
    return 1;
}
static inline int codec_clock_setup_divided(CodecClock *c, double host, int factor) {
    if (!codec_clock_setup(c, host))
        return 0;
    c->hz = codec_divide(c->hz, factor);
    return 1;
}
/* A fractional remainder survives vector and granule boundaries. */
static inline int codec_ticks(CodecClock *c) {
    ++c->sample;
    if (c->first) {
        c->first = 0;
        return 1;
    }
    c->phase += c->hz;
    if (c->phase < c->host)
        return 0;
    int n = (int)(c->phase / c->host);
    c->phase -= n * c->host;
    return n;
}
static inline int codec_sync(CodecClock *c, int ticks, int *pos, int period, int rising) {
    if (!rising)
        return ticks;
    double interval = period * c->host / c->hz;
    int plausible = !c->seen_edge || fabs((double)(c->sample - c->last_edge) - interval) <= 1.;
    c->seen_edge = 1;
    c->last_edge = c->sample;
    /* A bad sync signal must never keep rewinding an incomplete granule. */
    if (plausible && c->sample >= c->next_adjust &&
        !(ticks && (*pos % period == 0 || (*pos % period) + ticks > period))) {
        c->next_adjust = c->sample + (uint64_t)ceil(4 * interval);

        c->phase = 0;
        *pos -= *pos % period;
        return 1;
    }
    return ticks;
}
#endif
