#ifndef MPEG_SUBBAND_CONVERT_H
#define MPEG_SUBBAND_CONVERT_H
#include "filter.h"
#include "../codec_clock.h"
typedef struct {
    Format fmt;
    int factor, used;
    int bitrates[16];
    uint64_t source_slots, target_slots, emitted;
    float history[128][2][32];
    BandAudio target;
} Converter;
int convert_tables(void);
int convert_target(Format f, int divisor);
void convert_start(Converter *s, Format source, int target);
int convert_frame(Converter *s, const BandFrame *in, BandFrame *out);
int convert_finish(Converter *s, BandFrame *out);
#endif
