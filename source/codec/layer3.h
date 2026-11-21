#ifndef MP3PATCH_LAYER3_H
#define MP3PATCH_LAYER3_H

#include <stdint.h>
enum { L3_LINES = 576, L3_BANDS = 32, L3_FRAME_MAX = 2048 };
typedef struct {
    float q[L3_LINES], attenuation[L3_LINES];
    int gain, window;
} Fields;
typedef struct {
    unsigned char bytes[L3_FRAME_MAX];
    int length, channels, granules, rate, kbps;
    Fields fields[2][2]; /* granule, audio channel */
} EncodedFrame;
typedef struct {
    float lines[576];
    int window;
} Spectrum;
typedef struct {
    Spectrum gr[2][2];
    int channels, granules;
} SpectralFrame;
typedef struct Synthesis Synthesis;
void layer3_tables(void);
int layer3_frame_length(const unsigned char *header);
int layer3_unpack(EncodedFrame *out, const unsigned char *bytes, int length);
int layer3_header_rate(const unsigned char *header);
int layer3_read_fields(Synthesis *reader, const unsigned char *bytes, int length, EncodedFrame *out);
Synthesis *layer3_new(void);
int layer3_read_spectrum(Synthesis *s, const unsigned char *bytes, int length, int hz,
                         SpectralFrame *frame);
void layer3_render_spectrum(Synthesis *s, const Spectrum *spectrum, int ch, float *pcm);
void layer3_free(Synthesis *s);
void layer3_clear(Synthesis *s);
void layer3_split_mono(Synthesis *s);
void layer3_filter_basis(float bank[32][512], float analysis[18][36]);
void layer3_field_subbands(Synthesis *s, const Fields *f, int ch, float subbands[576]);
void layer3_long_fields(Fields *out, float lines[576], int hz);
void layer3_render(Synthesis *s, Fields *fields, int ch, float *pcm);
int layer3_decode_bytes(Synthesis *s, const unsigned char *bytes, int length, float *interleaved,
                        int *channels, int *rate);
#endif
