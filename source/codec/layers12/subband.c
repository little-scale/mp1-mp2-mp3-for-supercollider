#include "subband.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const int rates[2][3] = {{22050, 24000, 16000}, {44100, 48000, 32000}};
static const int kbtable[2][2][15] = {
    {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},
    {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
     {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384}}};
static float scalefactor[64];
void sb_tables(void) {
    for (int i = 0; i < 64; i++)
        scalefactor[i] = (float)(2 * exp2(-i / 3.));
}
int sb_clamp(double v, int lo, int hi) {
    if (!isfinite(v))
        return 0;
    if (v <= lo)
        return lo;
    if (v >= hi)
        return hi;
    return (int)round(v);
}
int sb_levels(int layer, int a) {
    if (!a)
        return 0;
    if (layer == 1)
        return (1 << (a + 1)) - 1;
    if (a <= 4)
        return 2 * a + 1;
    return (1 << (a - 1)) - 1;
}
static int rate_index(int hz) {
    for (int ver = 0; ver < 2; ver++)
        for (int i = 0; i < 3; i++)
            if (rates[ver][i] == hz)
                return 3 * ver + i;
    return -1;
}
static int legal(int layer, int mpeg1, int ch, int kbps) {
    if (layer == 2 && mpeg1) {
        if (ch == 1)
            return kbps <= 192;
        return kbps != 32 && kbps != 48 && kbps != 56 && kbps != 80;
    }
    return 1;
}
int sb_bitrate(int layer, int hz, int ch, double request, double ceiling) {
    int idx = rate_index(hz);
    if (idx < 0 || (ch != 1 && ch != 2) || (layer != 1 && layer != 2))
        return 0;
    const int *table = kbtable[idx / 3][layer - 1];
    if (!(request > 0))
        request = (layer == 1 ? 192 : 112) * ch;
    int best = 0;
    for (int i = 1; i < 15; i++)
        if (table[i] <= ceiling && legal(layer, idx / 3, ch, table[i]) &&
            (!best || fabs(table[i] - request) < fabs(best - request)))
            best = table[i];
    return best;
}
static int frame_bytes(Format f) {
    return f.layer == 1 ? 4 * ((12000 * f.kbps) / f.hz + f.pad) : (144000 * f.kbps) / f.hz + f.pad;
}
int sb_header(const uint8_t h[4], Format *f) {
    int ver = (h[1] >> 3) & 3, layer = 4 - ((h[1] >> 1) & 3), br = h[2] >> 4, sr = (h[2] >> 2) & 3;
    if (h[0] != 255 || (h[1] & 224) != 224 || ver < 2 || layer > 2 || !br || br == 15 || sr == 3 ||
        (h[3] & 3) == 2)
        return 0;
    int mode = h[3] >> 6, ch = mode == 3 ? 1 : 2, hz = rates[ver - 2][sr];
    *f = (Format){layer,
                  hz,
                  kbtable[ver - 2][layer - 1][br],
                  ch,
                  mode,
                  mode == 1 ? 4 + 4 * ((h[3] >> 4) & 3) : 32,
                  (h[2] >> 1) & 1,
                  hz};
    return frame_bytes(*f);
}
/* ISO allocation Tables B.2a–d and MPEG-2 Table B.1. Entries are quantizer
   classes in ascending precision; zero leaves the subband unallocated. */
typedef struct {
    int bands, width[32], max[32];
    const uint8_t *map[32];
} Layout;
static const uint8_t classmaps[][16] = {{0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17},
                                        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17},
                                        {0, 1, 2, 3, 4, 5, 6, 17},
                                        {0, 1, 2, 17},
                                        {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
                                        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
                                        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}};
static void layout(Format f, Layout *l) {
    int per = f.kbps / f.channels, low = f.hz >= 32000 && per < 56;
    l->bands = f.layer == 1                 ? 32
               : f.hz < 32000               ? 30
               : low                        ? (f.hz == 32000 ? 12 : 8)
               : per >= 96 && f.hz != 48000 ? 30
                                            : 27;
    for (int b = 0; b < l->bands; b++) {
        int map, width;
        if (f.layer == 1) {
            map = 6;
            width = 4;
        } else if (f.hz < 32000) {
            map = b < 4 ? 5 : 4;
            width = b < 4 ? 4 : b < 11 ? 3 : 2;
        } else if (low) {
            map = 4;
            width = b < 2 ? 4 : 3;
        } else {
            map = b < 3 ? 0 : b < 11 ? 1 : b < 23 ? 2 : 3;
            width = b < 11 ? 4 : b < 23 ? 3 : 2;
        }
        l->width[b] = width;
        l->max[b] = f.layer == 1 ? 14 : (1 << width) - 1;
        l->map[b] = classmaps[map];
    }
}
typedef struct {
    const uint8_t *in;
    uint8_t *out;
    int at, limit, error;
} Bits;
static unsigned readbits(Bits *s, int n) {
    if (s->at + n > s->limit) {
        s->error = 1;
        return 0;
    }
    unsigned v = 0;
    while (n--) {
        v = (v << 1) | ((s->in[s->at / 8] >> (7 - s->at % 8)) & 1);
        s->at++;
    }
    return v;
}
static void writebits(Bits *s, unsigned v, int n) {
    if (s->at + n > s->limit) {
        s->error = 1;
        return;
    }
    while (n--) {
        s->out[s->at / 8] |= ((v >> n) & 1) << (7 - s->at % 8);
        s->at++;
    }
}
static int grouped(int layer, int a) {
    return layer == 2 && (a == 1 || a == 2 || a == 4);
}
static int wordbits(int layer, int a) {
    if (layer == 1)
        return a + 1;
    if (a == 1)
        return 5;
    if (a == 2)
        return 7;
    if (a == 4)
        return 10;
    return a == 3 ? 3 : a - 1;
}
static int voices(Format f, int b) {
    return f.mode == 1 && b >= f.bound ? 1 : f.channels;
}
static int sfcode(const BandFrame *f, int ch, int b) {
    int a = f->scale[0][ch][b], m = f->scale[1][ch][b], z = f->scale[2][ch][b];
    return a == m ? (m == z ? 2 : 1) : m == z ? 3 : 0;
}
/* The CRC covers the last sixteen header bits, allocations and Layer II
   scalefactor selection bits. The stored checksum sits between those regions. */
static uint16_t crc_region(uint16_t crc, const uint8_t *data, int start, int end) {
    for (int bit = start; bit < end; bit++) {
        unsigned top = (crc >> 15) ^ ((data[bit / 8] >> (7 - bit % 8)) & 1);
        crc = (uint16_t)(crc << 1);
        if (top)
            crc ^= 0x8005;
    }
    return crc;
}
int sb_unpack(const uint8_t *data, int length, Packet *p) {
    Format fmt;
    if (length < 4 || length > SB_PACKET || sb_header(data, &fmt) != length)
        return 0;
    BandFrame *f = &p->fields;
    memset(f, 0, sizeof(*f));
    f->fmt = fmt;
    Layout l;
    layout(fmt, &l);
    Bits s = {.in = data, .at = (data[1] & 1) ? 32 : 48, .limit = 8 * length};
    int codes[2][32] = {{0}};
    for (int b = 0; b < l.bands; b++) {
        int n = voices(fmt, b);
        for (int c = 0; c < n; c++) {
            int index = readbits(&s, l.width[b]);
            if (index > l.max[b])
                return 0;
            f->allocation[c][b] = l.map[b][index];
        }
        if (n < fmt.channels)
            f->allocation[1][b] = f->allocation[0][b];
    }
    if (fmt.layer == 2)
        for (int b = 0; b < l.bands; b++)
            for (int c = 0; c < fmt.channels; c++)
                if (f->allocation[c][b])
                    codes[c][b] = readbits(&s, 2);
    if (s.error)
        return 0;
    if (!(data[1] & 1)) {
        if (length < 6)
            return 0;
        uint16_t crc = crc_region(0xffff, data, 16, 32);
        if (crc_region(crc, data, 48, s.at) != (uint16_t)((data[4] << 8) | data[5]))
            return 0;
    }
    for (int b = 0; b < l.bands; b++)
        for (int c = 0; c < fmt.channels; c++)
            if (f->allocation[c][b]) {
                int code = codes[c][b], a = readbits(&s, 6), m = a, z = a;
                if (fmt.layer == 2) {
                    if (code == 0 || code == 3)
                        m = readbits(&s, 6);
                    z = code == 2 || code == 3 ? m : (int)readbits(&s, 6);
                }
                f->scale[0][c][b] = a;
                f->scale[1][c][b] = m;
                f->scale[2][c][b] = z;
            }
    int step = fmt.layer == 1 ? 1 : 3;
    for (int row = 0; row < sb_rows(fmt.layer); row += step)
        for (int b = 0; b < l.bands; b++)
            for (int c = 0; c < voices(fmt, b); c++) {
                int a = f->allocation[c][b];
                if (!a)
                    continue;
                int levels = sb_levels(fmt.layer, a), n = wordbits(fmt.layer, a),
                    group = grouped(fmt.layer, a);
                unsigned triple = group ? readbits(&s, n) : 0;
                if (group && triple >= (unsigned)(levels * levels * levels))
                    return 0;
                for (int j = 0; j < step; j++) {
                    int word = group ? (int)(triple % levels) : (int)readbits(&s, n);
                    if (word >= levels)
                        return 0;
                    f->q[row + j][c][b] = word - levels / 2;
                    if (voices(fmt, b) < fmt.channels)
                        f->q[row + j][1][b] = f->q[row + j][0][b];
                    if (group)
                        triple /= levels;
                }
            }
    if (s.error)
        return 0;
    if (data != p->data)
        memcpy(p->data, data, length);
    p->length = length;
    p->fitted = 0;
    return 1;
}
float sb_amplitude(const BandFrame *f, int row, int ch, int b) {
    int a = f->allocation[ch][b];
    return a ? f->q[row][ch][b] * (2.f / sb_levels(f->fmt.layer, a)) *
                   scalefactor[f->scale[row / 12][ch][b]]
             : 0;
}
void sb_quantize(const BandAudio *src, Format fmt, BandFrame *f) {
    memset(f, 0, sizeof(*f));
    f->fmt = fmt;
    int rows = sb_rows(fmt.layer), max = fmt.layer == 1 ? 14 : 17,
        levels = sb_levels(fmt.layer, max);
    for (int c = 0; c < fmt.channels; c++)
        for (int b = 0; b < 32; b++)
            for (int g = 0; g < rows / 12; g++) {
                float peak = 0;
                for (int j = 0; j < 12; j++)
                    peak = fmaxf(peak, fabsf(src->x[g * 12 + j][c][b]));
                int sf = peak > 1e-14f ? sb_clamp(floor(-3 * log2(peak * .5)), 0, 62) : 62;
                f->scale[g][c][b] = sf;
                if (peak > 1e-14f)
                    f->allocation[c][b] = max;
                float inv = levels / (2 * scalefactor[sf]);
                for (int j = 0; j < 12; j++)
                    f->q[g * 12 + j][c][b] =
                        sb_clamp(src->x[g * 12 + j][c][b] * inv, -(levels / 2), levels / 2);
            }
}
typedef struct {
    int band, ch, voices, chosen, wanted, side;
    double energy;
} Unit;
static int cost(const Layout *l, const Unit *u, int code, int layer) {
    if (!code)
        return 0;
    int a = l->map[u->band][code];
    return u->side + wordbits(layer, a) * (grouped(layer, a) ? 12 : sb_rows(layer));
}
static int initial_budget(Format f, const Layout *l) {
    int bits = 8 * frame_bytes(f) - 32;
    for (int b = 0; b < l->bands; b++)
        bits -= l->width[b] * voices(f, b);
    return bits;
}
static int same_words(const BandFrame *f, int bound) {
    for (int b = bound; b < 32; b++) {
        if (f->allocation[0][b] != f->allocation[1][b])
            return 0;
        for (int j = 0; j < sb_rows(f->fmt.layer); j++)
            if (f->q[j][0][b] != f->q[j][1][b])
                return 0;
    }
    return 1;
}
/* An independent stereo band has two allocation decisions. A joint band
   has one decision and one set of words, with a separate scale on each side. */
static int make_units(const BandFrame *f, Format fmt, const Layout *l, Unit units[64], int budget) {
    int count = 0, total = 0;
    for (int b = 0; b < l->bands; b++)
        for (int c = 0; c < voices(fmt, b); c++) {
            Unit *u = &units[count++];
            *u = (Unit){.band = b, .ch = c, .voices = voices(fmt, b) == 1 ? fmt.channels : 1};
            int maxclass = 0;
            for (int k = c; k < c + u->voices; k++) {
                int a = f->allocation[k][b];
                if (a > maxclass)
                    maxclass = a;
                int sc = sfcode(f, k, b);
                u->side += fmt.layer == 1 ? 6 : 2 + 6 * (sc == 2 ? 1 : sc == 0 ? 3 : 2);
            }
            for (int i = 1; i <= l->max[b]; i++)
                if (l->map[b][i] <= maxclass)
                    u->wanted = i;
            total += cost(l, u, u->wanted, fmt.layer);
        }
    if (total <= budget) {
        for (int i = 0; i < count; i++)
            units[i].chosen = units[i].wanted;
        return count;
    }
    for (int i = 0; i < count; i++) {
        Unit *u = units + i;
        if (!u->wanted)
            continue;
        for (int c = u->ch; c < u->ch + u->voices; c++)
            for (int j = 0; j < sb_rows(fmt.layer); j++) {
                double v = sb_amplitude(f, j, c, u->band);
                u->energy += v * v;
            }
        /* A little low-band preference preserves pitch under very tight budgets. */
        u->energy = (u->energy + 1e-20) / (1 + .035 * u->band);
    }
    /* Spend the remaining bits on the largest reduction in weighted squared
       quantization error per bit, including the first band's side information. */
    for (;;) {
        int best = -1, extra = 0;
        double highest = -1;
        for (int i = 0; i < count; i++) {
            Unit *u = units + i;
            if (u->chosen >= u->wanted)
                continue;
            int next = u->chosen + 1,
                delta = cost(l, u, next, fmt.layer) - cost(l, u, u->chosen, fmt.layer);
            if (delta > budget)
                continue;
            double before = u->chosen ? sb_levels(fmt.layer, l->map[u->band][u->chosen]) : 1;
            double after = sb_levels(fmt.layer, l->map[u->band][next]);
            double priority = u->energy * (1 / (before * before) - 1 / (after * after)) / delta;
            if (priority > highest) {
                best = i;
                highest = priority;
                extra = delta;
            }
        }
        if (best < 0)
            break;
        units[best].chosen++;
        budget -= extra;
    }
    return count;
}
int sb_pack(const BandFrame *f, int requested, int keep_format, Padding *clock, Packet *p) {
    Format fmt = f->fmt;
    int index = rate_index(fmt.hz);
    if (index < 0 || (fmt.layer != 1 && fmt.layer != 2) || (fmt.channels != 1 && fmt.channels != 2))
        return 0;
    int kbps =
        sb_bitrate(fmt.layer, fmt.hz, fmt.channels, requested > 0 ? requested : fmt.kbps, 1000);
    if (!kbps)
        return 0;
    if (clock->hz != fmt.hz) {
        clock->hz = fmt.hz;
        clock->remainder = 0;
    }
    int product = (fmt.layer == 1 ? 12000 : 144000) * kbps;
    clock->remainder += product % fmt.hz;
    int pad = clock->remainder >= fmt.hz;
    if (pad)
        clock->remainder -= fmt.hz;
    fmt.pad = keep_format && kbps == fmt.kbps && (fmt.pad == 0 || fmt.pad == 1) ? fmt.pad : pad;
    fmt.kbps = kbps;
    if (fmt.channels == 1)
        fmt.mode = 3;
    else if (fmt.mode < 0 || fmt.mode > 2)
        fmt.mode = 0;
    if (fmt.mode == 1) {
        fmt.bound = 4 * sb_clamp(fmt.bound / 4., 1, 4);
        if (!same_words(f, fmt.bound))
            fmt.mode = 0;
    }
    if (fmt.mode != 1)
        fmt.bound = 32;
    Layout l;
    layout(fmt, &l);
    int budget = initial_budget(fmt, &l);
    if (budget < 0 && fmt.channels == 2) {
        fmt.mode = 1;
        fmt.bound = 4;
        budget = initial_budget(fmt, &l);
    }
    if (budget < 0)
        return 0;
    Unit u[64];
    int count = make_units(f, fmt, &l, u, budget);
    int n = frame_bytes(fmt);
    if (n > SB_PACKET)
        return 0;
    memset(p, 0, sizeof(*p));
    p->fields.fmt = fmt;
    p->length = n;
    const int *table = kbtable[index / 3][fmt.layer - 1];
    int bi = 1;
    while (table[bi] != kbps)
        bi++;
    p->data[0] = 255;
    p->data[1] = (uint8_t)(0xe1 | ((index / 3 + 2) << 3) | ((4 - fmt.layer) << 1));
    p->data[2] = (uint8_t)((bi << 4) | ((index % 3) << 2) | (fmt.pad << 1));
    p->data[3] = (uint8_t)((fmt.mode << 6) | (fmt.mode == 1 ? (fmt.bound / 4 - 1) << 4 : 0));
    Bits s = {.out = p->data, .at = 32, .limit = n * 8};
    BandFrame *out = &p->fields;
    for (int i = 0; i < count; i++) {
        Unit *v = u + i;
        int b = v->band, c = v->ch, a = l.map[b][v->chosen];
        writebits(&s, v->chosen, l.width[b]);
        for (int k = c; k < c + v->voices; k++) {
            out->allocation[k][b] = a;
            p->fitted += a != f->allocation[k][b];
            if (a)
                for (int g = 0; g < 3; g++)
                    out->scale[g][k][b] = f->scale[g][k][b] < 63 ? f->scale[g][k][b] : 62;
        }
        if (!a)
            continue;
        int levels = sb_levels(fmt.layer, a);
        for (int j = 0; j < sb_rows(fmt.layer); j++) {
            double num = 0, den = 0;
            for (int k = c; k < c + v->voices; k++) {
                double scale = scalefactor[out->scale[j / 12][k][b]];
                num += sb_amplitude(f, j, k, b) * scale;
                den += scale * scale;
            }
            int q = sb_clamp(num * levels / (2 * den), -levels / 2, levels / 2);
            for (int k = c; k < c + v->voices; k++)
                out->q[j][k][b] = q;
        }
    }
    for (int b = l.bands; b < 32; b++)
        for (int c = 0; c < fmt.channels; c++)
            p->fitted += f->allocation[c][b] != 0;
    if (fmt.layer == 2)
        for (int b = 0; b < l.bands; b++)
            for (int c = 0; c < fmt.channels; c++)
                if (out->allocation[c][b])
                    writebits(&s, sfcode(out, c, b), 2);
    for (int b = 0; b < l.bands; b++)
        for (int c = 0; c < fmt.channels; c++)
            if (out->allocation[c][b]) {
                int sc = sfcode(out, c, b);
                writebits(&s, out->scale[0][c][b], 6);
                if (fmt.layer == 2) {
                    if (sc == 0 || sc == 3)
                        writebits(&s, out->scale[1][c][b], 6);
                    if (sc == 0 || sc == 1)
                        writebits(&s, out->scale[2][c][b], 6);
                }
            }
    int step = fmt.layer == 1 ? 1 : 3;
    for (int j = 0; j < sb_rows(fmt.layer); j += step)
        for (int b = 0; b < l.bands; b++)
            for (int c = 0; c < voices(fmt, b); c++) {
                int a = out->allocation[c][b];
                if (!a)
                    continue;
                int levels = sb_levels(fmt.layer, a), nb = wordbits(fmt.layer, a),
                    center = levels / 2;
                if (grouped(fmt.layer, a)) {
                    int word = 0;
                    for (int k = 2; k >= 0; k--)
                        word = word * levels + out->q[j + k][c][b] + center;
                    writebits(&s, word, nb);
                } else
                    for (int k = 0; k < step; k++)
                        writebits(&s, out->q[j + k][c][b] + center, nb);
            }
    return !s.error;
}
