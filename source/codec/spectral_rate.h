#ifndef MP3PATCH_SPECTRAL_RATE_H
#define MP3PATCH_SPECTRAL_RATE_H
#include "layer3.h"
typedef struct SpectralRate SpectralRate;
/* One input granule per push. A converted frame is always a single LSF granule.
   Call finish until it returns zero to emit the remaining whole frames. */
SpectralRate *spectral_rate_new(int source_hz, int target_hz, int channels);
void spectral_rate_reset(SpectralRate *s);
void spectral_rate_free(SpectralRate *s);
int spectral_rate_fields(SpectralRate *s, const Fields in[2], Fields out[2]);
int spectral_rate_finish(SpectralRate *s, Fields out[2]);
#endif
