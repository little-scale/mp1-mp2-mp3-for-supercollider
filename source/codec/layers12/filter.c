#include "filter.h"
#include "window.h"
#include <math.h>
#include <string.h>
static float matrix[64][32], transpose[32][64], window[512];
static double energy;
void filter_impulse(int b, float impulse[512]) {
    for (int t = 0; t < 512; t++)
        impulse[t] = matrix[t % 64][b] * window[t];
}
double filter_energy(void) {
    return energy;
}
void filter_init(void) {
    double pi = acos(-1.);
    for (int i = 0; i < 64; i++)
        for (int b = 0; b < 32; b++)
            matrix[i][b] = cos((16 + i) * (2 * b + 1) * pi / 64);
    for (int i = 0; i <= 256; i++) {
        window[i] = window_half[i] / 65536.f;
        if (i && i < 256)
            window[512 - i] = i % 64 ? -window[i] : window[i];
    }
    float h[512];
    filter_impulse(0, h);
    energy = 0;
    for (int i = 0; i < 512; i++)
        energy += h[i] * h[i];
    for (int b = 0; b < 32; b++)
        for (int j = 0; j < 64; j++)
            transpose[b][j] = matrix[j][b] / energy;
}
/* Group the 512 prototype taps by cosine phase, then apply the transpose
   of the synthesis bank. The ring advances exactly one 32-sample slot. */
void filter_analyze(Analysis *a, const float pcm[32], float bands[32]) {
    memcpy(a->history[a->head], pcm, 32 * sizeof(float));
    a->head = (a->head + 1) & 15;
    float v[64] = {0};
    for (int age = 0; age < 16; age++)
        for (int j = 0; j < 32; j++)
            v[32 * (age & 1) + j] += a->history[(a->head + age) & 15][j] * window[32 * age + j];
    for (int b = 0; b < 32; b++) {
        float sum = 0;
        for (int j = 0; j < 64; j++)
            sum += v[j] * transpose[b][j];
        bands[b] = sum;
    }
}
void filter_synthesize(Synthesis *s, const float bands[32], float pcm[32]) {
    s->head = (s->head + 15) & 15;
    for (int j = 0; j < 64; j++) {
        float sum = 0;
        for (int b = 0; b < 32; b++)
            sum += matrix[j][b] * bands[b];
        s->history[s->head][j] = sum;
    }
    memset(pcm, 0, 32 * sizeof(float));
    for (int age = 0; age < 16; age++)
        for (int j = 0; j < 32; j++)
            pcm[j] += s->history[(s->head + age) & 15][32 * (age & 1) + j] * window[32 * age + j];
}
