#ifndef MP3PATCH_PACK_H
#define MP3PATCH_PACK_H
#include "layer3.h"
/* Fields are in synthesis order, with the same rounded ranges as mp3decode~. */
typedef struct {
    int rate, channels, bitrate, remainder, previous[2];
    unsigned long frames, fitted, windows;
} Pack;
int pack_init(Pack *p, int rate, int channels, int bitrate);
int pack_frame(Pack *p, const Fields fields[2][2], unsigned char out[L3_FRAME_MAX]);
int pack_file_frame(Pack *p, const Fields fields[2][2], unsigned char out[L3_FRAME_MAX]);
#endif
