#ifndef MPEG_SUBBAND_H
#define MPEG_SUBBAND_H
#include <stdint.h>
enum { SB_BANDS = 32, SB_ROWS = 36, SB_PACKET = 2048 };
typedef struct {
    int layer, hz, kbps, channels, mode, bound, pad, original_hz;
} Format;
typedef struct {
    Format fmt;
    uint8_t allocation[2][32], scale[3][2][32];
    int16_t q[36][2][32];
} BandFrame;
typedef struct {
    float x[36][2][32];
} BandAudio;
typedef struct {
    BandFrame fields;
    uint8_t data[SB_PACKET];
    int length, fitted;
} Packet;
typedef struct {
    int hz, remainder;
} Padding;
static inline int sb_rows(int layer) {
    return layer == 1 ? 12 : 36;
}
int sb_levels(int layer, int allocation);
int sb_bitrate(int layer, int hz, int channels, double request, double ceiling);
int sb_header(const uint8_t header[4], Format *f);
int sb_unpack(const uint8_t *data, int length, Packet *p);
int sb_pack(const BandFrame *f, int requested_kbps, int keep_format, Padding *clock, Packet *out);
void sb_tables(void);
float sb_amplitude(const BandFrame *f, int slot, int ch, int band);
void sb_quantize(const BandAudio *audio, Format fmt, BandFrame *f);
int sb_clamp(double value, int low, int high);
#endif
