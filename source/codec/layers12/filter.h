#ifndef MPEG_SUBBAND_FILTER_H
#define MPEG_SUBBAND_FILTER_H
#include "subband.h"
typedef struct {
    float history[16][32];
    int head;
} Analysis;
typedef struct {
    float history[16][64];
    int head;
} Synthesis;
void filter_init(void);
void filter_analyze(Analysis *a, const float pcm[32], float bands[32]);
void filter_synthesize(Synthesis *s, const float bands[32], float pcm[32]);
void filter_impulse(int band, float impulse[512]);
double filter_energy(void);
#endif
