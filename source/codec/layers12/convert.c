#include "convert.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
enum { MAX_FACTOR = 3, HALF_TAPS = 48, BANK_LENGTH = 512 };
typedef struct {
    int16_t lag;
    uint8_t from, to;
    float gain;
} Tap;
typedef struct {
    int factor, lookahead, tap_count;
    Tap *taps;
} Kernel;
static Kernel kernels[4];
/* Project each source synthesis impulse through the anti-alias lowpass
   and directly onto each destination analysis filter. Runtime operates on
   subband samples and never constructs a full-band PCM buffer. */
static int make_kernel(Kernel *s) {
    float bank[32][BANK_LENGTH];
    for (int b = 0; b < 32; ++b)
        filter_impulse(b, bank[b]);
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
                /* The sparse matrix
                   omits individual couplings below one part in a million. */
                float gain = (float)(dot / filter_energy());
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
int convert_tables(void) {
    for (int f = 2; f <= 3; f++) {
        Kernel *k = kernels + f;
        if (k->taps)
            continue;
        k->factor = f;
        if (!make_kernel(k)) {
            free(k->taps);
            *k = (Kernel){0};
            return 0;
        }
    }
    return 1;
}
int convert_target(Format f, int divisor) {
    int used = f.original_hz >= f.hz && f.hz ? f.original_hz / f.hz : 1;
    return codec_divide(f.hz, divisor / used);
}
void convert_start(Converter *s, Format src, int target) {
    memset(s, 0, sizeof(*s));
    s->fmt = src;
    s->factor = src.hz / target;
    s->fmt.hz = target;
    s->fmt.kbps = sb_bitrate(src.layer, target, src.channels, src.kbps, 1000);
    s->fmt.mode = src.channels == 1 ? 3 : 0;
    s->fmt.bound = 32;
    s->fmt.pad = -1;
}
static int next_slot(Converter *s, BandFrame *out) {
    float(*dst)[32] = s->target.x[s->used];
    memset(dst, 0, 2 * 32 * sizeof(float));
    const Kernel *k = kernels + s->factor;
    int64_t center = (int64_t)s->target_slots * s->factor;
    for (int i = 0; i < k->tap_count; i++) {
        const Tap *t = k->taps + i;
        int64_t at = center - t->lag;
        if (at < 0 || (uint64_t)at >= s->source_slots)
            continue;
        const float(*v)[32] = s->history[at % 128];
        for (int c = 0; c < s->fmt.channels; c++)
            dst[c][t->to] += t->gain * v[c][t->from];
    }
    s->target_slots++;
    if (++s->used != sb_rows(s->fmt.layer))
        return 0;
    s->fmt.kbps = s->bitrates[s->emitted % 16];
    sb_quantize(&s->target, s->fmt, out);
    s->used = 0;
    s->emitted++;
    return 1;
}
int convert_frame(Converter *s, const BandFrame *in, BandFrame *out) {
    if (s->factor == 1) {
        *out = *in;
        return 1;
    }
    int ready = 0, rows = sb_rows(in->fmt.layer);
    /* Center each output frame on its corresponding group of input frames,
       even though the anti-alias filter needs additional input lookahead. */
    uint64_t source_frame = s->source_slots / rows;
    if (source_frame % s->factor == 0)
        s->bitrates[source_frame / s->factor % 16] =
            sb_bitrate(in->fmt.layer, s->fmt.hz, in->fmt.channels, in->fmt.kbps, 1000);
    for (int j = 0; j < rows; j++) {
        float(*d)[32] = s->history[s->source_slots++ % 128];
        for (int c = 0; c < in->fmt.channels; c++)
            for (int b = 0; b < 32; b++)
                d[c][b] = sb_amplitude(in, j, c, b);
        while (s->target_slots * s->factor + kernels[s->factor].lookahead < s->source_slots)
            ready |= next_slot(s, out);
    }
    return ready;
}
int convert_finish(Converter *s, BandFrame *out) {
    if (s->factor < 2 || s->emitted * sb_rows(s->fmt.layer) * s->factor >= s->source_slots)
        return 0;
    while (!next_slot(s, out)) {
    }
    return 1;
}
