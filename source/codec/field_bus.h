#ifndef MP3PATCH_FIELD_BUS_H
#define MP3PATCH_FIELD_BUS_H
#include "layer3.h"
#include <math.h>
enum { BAND_HOLD_INLET = 5, FIELD_INLETS = 6 };
typedef struct {
    long width[FIELD_INLETS], offset[FIELD_INLETS];
    short connected[FIELD_INLETS];
} FieldBus;
static inline int bus_channels(const FieldBus *b) {
    return 1 + (b->width[0] > 32 || b->width[1] > 32 || b->width[2] > 1 || b->width[3] > 1 ||
                b->width[BAND_HOLD_INLET] > 32);
}
static inline double bus_read(const FieldBus *b, double **in, int port, int ch, int band, long i,
                              double fallback) {
    if (!b->connected[port])
        return fallback;
    long width = b->width[port];
    long lane = width == 1 ? 0 : (port == 2 || port == 3) ? ch : width > 32 ? 32 * ch + band : band;
    return lane < width ? in[b->offset[port] + lane][i] : fallback;
}
/* One gate per hybrid subband, shared or independently addressed in stereo. */
static inline uint32_t bus_hold_mask(const FieldBus *b, int port, double **in, int ch, long i) {
    if (!b->connected[port])
        return 0;
    uint32_t mask = 0;
    for (int band = 0; band < 32; ++band) {
        double gate = bus_read(b, in, port, ch, band, i, 0);
        if (isfinite(gate) && gate >= 0.5)
            mask |= UINT32_C(1) << band;
    }
    return mask;
}
static inline int bus_limit(double value, int lo, int hi, unsigned *limited) {
    if (!isfinite(value)) {
        ++*limited;
        return 0;
    }
    if (value < lo) {
        ++*limited;
        return lo;
    }
    if (value > hi) {
        ++*limited;
        return hi;
    }
    return (int)round(value);
}
/* Express a captured line under a shared granule gain. Only values beyond
   the attenuation field's range require a change to the quantized integer. */
static inline void field_rebase(Fields *dst, const Fields *src, int j, int source_gain) {
    int attenuation = (int)src->attenuation[j] + dst->gain - source_gain;
    int stored = attenuation > 210 ? 210 : attenuation < 0 ? 0 : attenuation;
    double q = src->q[j];
    if (q && attenuation != stored)
        q = fmax(-8206., fmin(8206., round(q * exp2((stored - attenuation) * (3. / 16.)))));
    dst->q[j] = (float)q;
    dst->attenuation[j] = stored;
}
static inline void bus_capture(const FieldBus *b, double **in, Fields *f, const Fields *previous,
                               uint32_t mask, int ch, int pos, long i, unsigned *limited) {
    if (!pos) {
        f->gain = bus_limit(bus_read(b, in, 2, ch, 0, i, 160), 0, 255, limited);
        f->window = bus_limit(bus_read(b, in, 3, ch, 0, i, 0), 0, 5, limited);
    }
    for (int k = 0; k < 32; k++) {
        int j = 18 * k + pos / 32;
        if (mask & (UINT32_C(1) << k)) {
            f->q[j] = previous->q[j];
            f->attenuation[j] = previous->attenuation[j];
            continue;
        }
        f->q[j] = bus_limit(bus_read(b, in, 0, ch, k, i, 0), -8206, 8206, limited);
        f->attenuation[j] = bus_limit(bus_read(b, in, 1, ch, k, i, 0), 0, 210, limited);
    }
}
#endif
