#include "spectral_rate.h"
#include "layer3_layout.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Compose the matched analysis/synthesis subband filters with a low-pass FIR.
   The resulting taps directly map incoming subband slots to lower-rate slots. */
enum { HISTORY = 256, MAX_FACTOR = 6, HALF_TAPS = 48, BANK_LENGTH = 512 };
typedef struct {
    int16_t lag;
    uint8_t from, to;
    float gain;
} Tap;
struct SpectralRate {
    Synthesis *hybrid;
    int rate, factor, channels, lookahead, tap_count, window_slots;
    int64_t source_slots, target_slots, written;
    Tap *taps;
    float history[HISTORY][2][32], window[2][32][36], analysis[18][36], block[2][576];
};
static int make_kernel(SpectralRate *s) {
    float bank[32][BANK_LENGTH];
    layer3_filter_basis(bank, s->analysis);
    int factor = s->factor, radius = HALF_TAPS * factor, length = BANK_LENGTH + 2 * radius;
    float *filtered = calloc(32 * length, sizeof(float));
    if (!filtered)
        return 0;

    double pi = acos(-1.), cutoff = .94 / factor, lowpass[2 * HALF_TAPS * MAX_FACTOR + 1], sum = 0;
    for (int i = -radius; i <= radius; ++i) {
        double win = .42 + .5 * cos(pi * i / radius) + .08 * cos(2 * pi * i / radius);
        lowpass[i + radius] = win * (i ? sin(pi * cutoff * i) / (pi * i) : cutoff);
        sum += lowpass[i + radius];
    }
    for (int band = 0; band < 32; ++band) {
        float *impulse = filtered + band * length;
        for (int j = 0; j <= 2 * radius; ++j)
            for (int i = 0; i < BANK_LENGTH; ++i)
                impulse[i + j] += bank[band][i] * (float)(lowpass[j] / sum);
    }

    int last_sample = BANK_LENGTH - 1;
    int first = -(factor * last_sample + radius) / 32, last = (last_sample + radius) / 32;
    s->lookahead = -first;
    int capacity = 4096;
    s->taps = malloc(capacity * sizeof(Tap));
    if (!s->taps) {
        free(filtered);
        return 0;
    }
    for (int lag = first; lag <= last; ++lag) {
        int base = 32 * lag + radius;
        int start = base < 0 ? (-base + factor - 1) / factor : 0;
        int end =
            base + factor * last_sample < length ? BANK_LENGTH : (length - 1 - base) / factor + 1;
        for (int from = 0; from < 32; ++from) {
            float impulse[BANK_LENGTH];
            for (int j = start; j < end; ++j)
                impulse[j] = filtered[from * length + base + factor * j];
            for (int to = 0; to < 32; ++to) {
                double dot = 0;
                for (int j = start; j < end; ++j)
                    dot += (double)bank[to][j] * impulse[j];
                /* Each synthesis impulse has energy 128. The sparse matrix
                   omits individual couplings below one part in a million. */
                float gain = (float)(dot / 128.);
                if (fabsf(gain) < 1.e-6f)
                    continue;
                if (s->tap_count == capacity) {
                    capacity *= 2;
                    Tap *grown = realloc(s->taps, capacity * sizeof(Tap));
                    if (!grown) {
                        free(filtered);
                        return 0;
                    }
                    s->taps = grown;
                }
                s->taps[s->tap_count++] = (Tap){(int16_t)lag, (uint8_t)from, (uint8_t)to, gain};
            }
        }
    }
    free(filtered);
    return 1;
}
SpectralRate *spectral_rate_new(int source_hz, int target_hz, int channels) {
    int version, frequency;
    if (!layer3_layout(source_hz, &version, &frequency) ||
        !layer3_layout(target_hz, &version, &frequency) || source_hz <= target_hz ||
        source_hz % target_hz || source_hz / target_hz > MAX_FACTOR || channels < 1 || channels > 2)
        return NULL;
    SpectralRate *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->factor = source_hz / target_hz;
    s->rate = target_hz;
    s->channels = channels;
    s->hybrid = layer3_new();
    if (!s->hybrid || !make_kernel(s)) {
        spectral_rate_free(s);
        return NULL;
    }
    return s;
}
void spectral_rate_reset(SpectralRate *s) {
    if (!s)
        return;
    layer3_clear(s->hybrid);
    s->source_slots = s->target_slots = s->written = 0;
    s->window_slots = 0;
    memset(s->window, 0, sizeof(s->window));
}
void spectral_rate_free(SpectralRate *s) {
    if (!s)
        return;
    layer3_free(s->hybrid);
    free(s->taps);
    free(s);
}
static void target_slot(SpectralRate *s) {
    float bands[2][32] = {{0}};
    int64_t center = s->target_slots * s->factor;
    for (int i = 0; i < s->tap_count; ++i) {
        const Tap *t = &s->taps[i];
        int64_t at = center - t->lag;
        if (at < 0 || at >= s->source_slots)
            continue;
        const float(*source)[32] = s->history[at % HISTORY];
        for (int c = 0; c < s->channels; ++c)
            bands[c][t->to] += t->gain * source[c][t->from];
    }
    /* Invert the odd-band, odd-slot modulation before target MDCT analysis. */
    for (int c = 0; c < s->channels; ++c)
        for (int b = 0; b < 32; ++b)
            s->window[c][b][s->window_slots] =
                (s->target_slots & 1) && (b & 1) ? -bands[c][b] : bands[c][b];
    ++s->target_slots;
    ++s->window_slots;
}
static void take_granule(SpectralRate *s, Fields out[2]) {
    for (int c = 0; c < s->channels; ++c) {
        float lines[576];
        for (int b = 0; b < 32; ++b) {
            float *window = s->window[c][b];
            for (int j = 0; j < 18; ++j) {
                double dot = 0;
                for (int n = 0; n < 36; ++n)
                    dot += s->analysis[j][n] * window[n];
                lines[18 * b + j] = (float)dot;
            }
            memmove(window, window + 18, 18 * sizeof(float));
        }
        layer3_long_fields(out + c, lines, s->rate);
    }
    s->window_slots = 18;
    ++s->written;
}
int spectral_rate_fields(SpectralRate *s, const Fields in[2], Fields out[2]) {
    for (int c = 0; c < s->channels; ++c)
        layer3_field_subbands(s->hybrid, in + c, c, s->block[c]);
    for (int n = 0; n < 18; ++n) {
        float(*slot)[32] = s->history[s->source_slots++ % HISTORY];
        for (int c = 0; c < s->channels; ++c)
            for (int b = 0; b < 32; ++b)
                slot[c][b] = s->block[c][18 * b + n];
    }
    int emitted = 0;
    while (s->target_slots * s->factor + s->lookahead < s->source_slots) {
        target_slot(s);
        if (s->window_slots == 36) {
            take_granule(s, out);
            emitted = 1;
        }
    }
    return emitted;
}
int spectral_rate_finish(SpectralRate *s, Fields out[2]) {
    if (s->written * 18 * s->factor >= s->source_slots)
        return 0;
    while (s->window_slots < 36)
        target_slot(s);
    take_granule(s, out);
    return 1;
}
