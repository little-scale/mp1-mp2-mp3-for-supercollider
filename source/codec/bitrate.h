#ifndef MP3PATCH_BITRATE_H
#define MP3PATCH_BITRATE_H
#include <math.h>
/* Header indices 1 through 14, MPEG-2/2.5 and MPEG-1. */
static const int br[2][15] = {{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
                              {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};
static inline int native_bitrate(int hz, double request, double ceiling, int fallback) {
    if (!hz)
        return 0;
    const int *table = br[hz >= 32000];
    int best = 0, last = hz < 16000 ? 8 : 14;
    double target = request > 0 ? request : fallback;
    for (int i = 1; i <= last && table[i] <= ceiling; i++) {
        if (!best || fabs(table[i] - target) < fabs(best - target))
            best = table[i];
    }
    return best;
}
static inline int pack_bitrate(int hz, double request) {
    return native_bitrate(hz, request, 320, hz >= 32000 ? 320 : hz >= 16000 ? 160 : 64);
}
static inline int live_bitrate(int hz, double host, double request) {
    return native_bitrate(hz, request, fmin(hz, host) * .008,
                          hz >= 32000   ? 128
                          : hz >= 16000 ? 64
                                        : 32);
}
#endif
