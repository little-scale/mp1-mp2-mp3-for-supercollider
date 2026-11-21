#ifndef MP3PATCH_RATE_H
#define MP3PATCH_RATE_H
#include "codec_clock.h"
#include "fractional.h"
#include <math.h>
#include <string.h>
typedef struct {
    int hz, factor, taps, head;
    CodecClock clock;
    float fir[241], delay[2][241];
} Rate;
static inline int rate_setup_native(Rate *r, double sample_rate, int native) {
    memset(r, 0, sizeof(*r));
    if (!codec_clock_setup(&r->clock, sample_rate))
        return 0;
    if (native) r->clock.hz = native;
    r->hz = r->clock.hz;
    for (int f = 1; f <= 6; ++f)
        if (fabs(sample_rate - r->hz * f) < 1.e-7) {
            r->factor = f;
            r->taps = 40 * f + 1;
            if (f == 1)
                return 1;
            double sum = 0, pi = acos(-1.0);
            for (int k = 0; k < r->taps; ++k) {
                int n = k - 20 * f;
                double w = .42 - .5 * cos(2 * pi * k / (r->taps - 1)) +
                           .08 * cos(4 * pi * k / (r->taps - 1));
                double lp = n ? sin(pi * .94 * n / f) / (pi * n) : .94 / f;
                r->fir[k] = (float)(w * lp);
                sum += w * lp;
            }
            for (int k = 0; k < r->taps; ++k)
                r->fir[k] /= (float)sum;
            return 1;
        }
    r->factor = -1;
    return 1;
}
static inline int rate_setup(Rate *r, double host) { return rate_setup_native(r, host, 0); }
static inline void rate_push(Rate *r, float l, float rr) {
    if (--r->head < 0)
        r->head = r->taps - 1;
    r->delay[0][r->head] = l;
    r->delay[1][r->head] = rr;
}
static inline float rate_filter(const Rate *r, int ch) {
    double sum = 0;
    int at = r->head;
    for (int i = 0; i < r->taps; ++i) {
        sum += r->fir[i] * r->delay[ch][at];
        if (++at == r->taps)
            at = 0;
    }
    return (float)sum;
}
/* Integer reconstruction inserts zeros between native ticks. Only this
   phase of the FIR can contribute to the current host sample. */
static inline float rate_interpolate(const Rate *r, int ch) {
    int age = (int)(r->clock.phase / r->hz);
    double sum = 0;
    for (int j = age; j < r->taps; j += r->factor) {
        int at = r->head + j;
        if (at >= r->taps)
            at -= r->taps;
        sum += r->fir[j] * r->delay[ch][at];
    }
    return (float)(r->factor * sum);
}
#endif
