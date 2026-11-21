#ifndef MP3PATCH_FRACTIONAL_H
#define MP3PATCH_FRACTIONAL_H
#include <math.h>
#include <stdlib.h>
/* Windowed-sinc conversion for clocks outside the native and 2x/4x paths.
   Allocate on DSP setup. The audio callback only reads coefficients. */
enum { PHASES = 128 };
typedef struct {
    int taps, head;
    float *kernel, *history;
} Fractional;
static inline void fractional_free(Fractional *f) {
    free(f->kernel);
    free(f->history);
    *f = (Fractional){0};
}
static inline int fractional_setup(Fractional *f, double source, double dest) {
    double ratio = fmin(1., dest / source), cut = .94 * ratio, pi = acos(-1.);
    int n = (int)fmin(1023., 2 * ceil(20. / ratio) + 1);
    f->kernel = (float *)calloc(n * (2 * PHASES + 1), sizeof(float));
    f->history = (float *)calloc(2 * n, sizeof(float));
    f->taps = n;
    if (!f->kernel || !f->history) {
        fractional_free(f);
        return 0;
    }
    for (int phase = -PHASES; phase <= PHASES; phase++) {
        float *k = f->kernel + (phase + PHASES) * n;
        double sum = 0;
        for (int j = 0; j < n; j++) {
            double t = j - (n - 1) * .5 - (double)phase / PHASES;
            double win = .42 - .5 * cos(2 * pi * j / (n - 1)) + .08 * cos(4 * pi * j / (n - 1));
            sum += (k[j] = (float)(win * (fabs(t) < 1.e-12 ? cut : sin(pi * cut * t) / (pi * t))));
        }
        for (int j = 0; j < n; j++)
            k[j] /= (float)sum;
    }
    return 1;
}
static inline void fractional_push(Fractional *f, float l, float r) {
    if (--f->head < 0)
        f->head = f->taps - 1;
    f->history[f->head] = l;
    f->history[f->head + f->taps] = r;
}
static inline float fractional_read(Fractional *f, int ch, double age) {
    int phase = (int)round(fmin(1., fmax(-1., age)) * PHASES);
    const float *k = f->kernel + (phase + PHASES) * f->taps, *v = f->history + ch * f->taps;
    double sum = 0;
    int j = f->head;
    for (int i = 0; i < f->taps; i++) {
        sum += k[i] * v[j];
        if (++j == f->taps)
            j = 0;
    }
    return (float)sum;
}
#endif
