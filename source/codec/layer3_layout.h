#ifndef MP3PATCH_LAYER3_LAYOUT_H
#define MP3PATCH_LAYER3_LAYOUT_H
/* The first two MPEG-2.5 frequency indices use the same scalefactor layout. */
static inline int layer3_band_index(int version, int frequency) {
    int index = 3 * version + frequency;
    return index ? index - 1 : 0;
}
static inline int layer3_layout(int rate, int *version, int *freq) {
    static const int rates[3][3] = {
        {11025, 12000, 8000}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    for (int v = 0; v < 3; v++)
        for (int i = 0; i < 3; i++)
            if (rate == rates[v][i]) {
                *version = v;
                *freq = i;
                return 1;
            }
    return 0;
}
#endif
