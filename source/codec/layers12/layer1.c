#include "layer1.h"
#include "window.h"
#include <math.h>
#include <string.h>

static double matrix[64][32], synthesis_window[512], scales[63];

void layer1_init_tables(void) {
    const double pi = acos(-1.0);
    for (int row = 0; row < 64; ++row)
        for (int band = 0; band < 32; ++band)
            matrix[row][band] = cos((16 + row) * (2 * band + 1) * pi / 64);
    for (int i = 0; i <= 256; ++i) {
        double coefficient = window_half[i] / 65536.0;
        synthesis_window[i] = coefficient;
        if (i > 0 && i < 256)
            synthesis_window[512 - i] = i % 64 ? -coefficient : coefficient;
    }
    for (int i = 0; i < 63; ++i)
        scales[i] = 2.0 * exp2(-i / 3.0);
}

void layer1_reset(Layer1 *d) {
    memset(d, 0, sizeof(*d));
}

static int field(double v, int maximum) {
    if (!(v > 0)) return 0;
    if (v >= maximum) return maximum;
    return (int)(v + 0.5);
}

static void synthesize(Layer1 *d, int raw) {
    double subbands[32] = {0};
    for (int b = 0; b < 32; ++b) {
        const int allocation = d->allocation[b];
        if (allocation == 0 || !(d->written & (UINT32_C(1) << b))) continue;
        const int center = (1 << allocation) - 1;
        const double v = d->input[b];
        const int word = raw ? field(v, 2 * center)
                             : field((1.0 + v) * center, 2 * center);
        subbands[b] = (word - center) * d->step[b];
    }
    /* ISO/IEC 11172-3 synthesis matrix, then the polyphase window sum. */
    d->head = (d->head + 15) & 15;
    for (int row = 0; row < 64; ++row) {
        double sum = 0;
        for (int b = 0; b < 32; ++b)
            sum += matrix[row][b] * subbands[b];
        d->history[d->head][row] = sum;
    }
    memset(d->pcm, 0, sizeof(d->pcm));
    for (int age = 0; age < 16; ++age) {
        const double *values = d->history[(d->head + age) & 15] + (age % 2) * 32;
        const double *window = synthesis_window + age * 32;
        for (int j = 0; j < 32; ++j)
            d->pcm[j] += values[j] * window[j];
    }
    d->written = 0;
}

double layer1_tick(Layer1 *d, double data, double address, double allocation,
                   double scale, int raw, int data_connected) {
    const int column = d->column;
    const double output = d->pcm[column];
    if (d->slot == 0) {
        const int a = field(allocation, 14);
        d->allocation[column] = a;
        d->step[column] = 2.0 * scales[field(scale, 62)] / ((1 << (a + 1)) - 1);
    }
    if (data_connected && isfinite(data)) {
        const int band = !isfinite(address) || address < 0 ? column
                       : address >= 31 ? 31 : (int)address;
        d->input[band] = data;
        d->written |= UINT32_C(1) << band;
    }
    if (++d->column == 32) {
        synthesize(d, raw);
        d->column = 0;
        d->slot = (d->slot + 1) % 12;
    }
    return output;
}
