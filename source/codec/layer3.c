#include "layer3.h"
#include "layer3_layout.h"
#include "bitrate.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_SIMD
#include "vendor/minimp3.h"
#include <math.h>
#include "vendor/pack_layout.h"

struct Synthesis {
    mp3dec_t packed;
    float qmf[2][15 * 64], tail[2][576];
    float lines[576], work[33 * 64];
};
static float power43[8207], exponent[512], tail_window[2][18];

void layer3_tables(void) {
    for (int i = 0; i <= 8206; ++i)
        power43[i] = L3_pow_43(i);
    for (int i = 0; i < 512; ++i)
        exponent[i] = exp2f((i - 420) * 0.25f);
    double pi = acos(-1.0);
    for (int i = 0; i < 18; ++i) {
        tail_window[0][i] = (float)cos(pi * (i + 0.5) / 36);
        tail_window[1][i] = i < 6 ? 1 : i >= 12 ? 0 : (float)cos(pi * (i - 5.5) / 12);
    }
}
Synthesis *layer3_new(void) { return calloc(1, sizeof(Synthesis)); }
void layer3_free(Synthesis *s) { free(s); }
void layer3_clear(Synthesis *s) { memset(s, 0, sizeof(*s)); }
/* Seed the second filter bank when an MC mask splits mono playback. */
void layer3_split_mono(Synthesis *s) {
    memcpy(s->qmf[1], s->qmf[0], sizeof(s->qmf[0]));
    memcpy(s->tail[1], s->tail[0], sizeof(s->tail[0]));
}
int layer3_frame_length(const unsigned char *h) {
    if (!hdr_valid(h) || HDR_GET_LAYER(h) != 1 || HDR_IS_FREE_FORMAT(h))
        return 0;
    int n = hdr_frame_bytes(h, 0) + hdr_padding(h);
    return n >= 5 && n <= L3_FRAME_MAX ? n : 0;
}

int layer3_header_rate(const unsigned char *h) {
    return layer3_frame_length(h) ? (int)hdr_sample_rate_hz(h) : 0;
}
static void field_header(EncodedFrame *out, const unsigned char *h, int n) {
    out->channels = HDR_IS_MONO(h) ? 1 : 2;
    out->granules = HDR_TEST_MPEG1(h) ? 2 : 1;
    out->rate = (int)hdr_sample_rate_hz(h);
    out->kbps = br[out->rate >= 32000][h[2] >> 4];
    out->length = n;
    memcpy(out->bytes, h, n);
}
/* Unity-scale Huffman decoding recovers the original signed quantizer integers. */
static int independent_fields(EncodedFrame *out, const unsigned char *bytes,
                              mp3dec_scratch_t *scratch, bs_t *bs) {
    float unity[40];
    for (int i = 0; i < 40; ++i)
        unity[i] = 1;
    for (int gr = 0; gr < out->granules; ++gr)
        for (int ch = 0; ch < out->channels; ++ch) {
            Fields *f = &out->fields[gr][ch];
            L3_gr_info_t info = scratch->gr_info[gr * out->channels + ch];
            int end = bs->pos + info.part_23_length;
            if (end > bs->limit)
                return 0;
            f->gain = info.global_gain;
            int long_bands =
                info.mixed_block_flag ? (2 << (HDR_GET_MY_SAMPLE_RATE(bytes) == 2)) : 0;
            f->window =
                info.block_type == 2 && long_bands ? (long_bands == 4 ? 5 : 4) : info.block_type;
            info.global_gain = 210 - 4 * BITS_DEQUANTIZER_OUT; /* unit overall scale */
            L3_decode_scalefactors(bytes, scratch->ist_pos[ch], bs, &info, scratch->scf, ch);
            if (bs->pos > end)
                return 0;
            memset(f->q, 0, sizeof(f->q));
            L3_huffman(f->q, bs, &info, unity, end);
            for (int i = 0; i < 576; ++i) {
                float v = f->q[i];
                if (v)
                    f->q[i] = copysignf(roundf(powf(fabsf(v), 0.75f)), v);
            }
            int line = 0;
            for (int k = 0; k < info.n_long_sfb + info.n_short_sfb; ++k) {
                float steps = roundf(-4 * log2f(scratch->scf[k]));
                int width = info.sfbtab[k];
                if (line + width > 576)
                    return 0;
                for (int i = 0; i < width; ++i)
                    f->attenuation[line++] = steps;
            }
            if (line != 576)
                return 0;
            if (info.n_short_sfb) {
                int start = 18 * long_bands;
                L3_reorder(f->q + start, scratch->syn[0], info.sfbtab + info.n_long_sfb);
                L3_reorder(f->attenuation + start, scratch->syn[0], info.sfbtab + info.n_long_sfb);
            }
        }
    return 1;
}

int layer3_unpack(EncodedFrame *out, const unsigned char *h, int n) {
    if (n < 5 || layer3_frame_length(h) != n ||
        (HDR_GET_STEREO_MODE(h) == MODE_JOINT_STEREO && HDR_GET_STEREO_MODE_EXT(h)))
        return 0;
    mp3dec_scratch_t scratch = {0};
    bs_t bs;
    bs_init(&bs, h + 4, n - 4);
    if (HDR_IS_CRC(h))
        get_bits(&bs, 16);
    if (L3_read_side_info(&bs, scratch.gr_info, h) != 0 || bs.pos > bs.limit)
        return 0;
    int side = bs.pos / 8;
    memcpy(scratch.maindata, h + 4 + side, n - 4 - side);
    bs_init(&bs, scratch.maindata, n - 4 - side);
    field_header(out, h, n);
    return independent_fields(out, h, &scratch, &bs);
}

static void hybrid(Synthesis *s, int window, int ch) {
    /* The 8 kHz layout has 72 long-order lines but uses two long IMDCT subbands. */
    int mixed = window >= 4 ? 2 : 0;
    int block = mixed ? 2 : window;
    L3_antialias(s->lines, block == 2 ? mixed - 1 : 31);
    /* A patch can switch between pure short and mixed on successive granules.
       Window the outgoing overlap now, using the layout that produced it. */
    float overlap[32 * 9] = {0};
    L3_imdct_gr(s->lines, overlap, block, mixed);
    for (int band = 0; band < 32; ++band) {
        float *lines = s->lines + 18 * band, *tail = s->tail[ch] + 18 * band;
        const float *half = overlap + 9 * band;
        const float *win = tail_window[block == 1 || (block == 2 && band >= mixed)];
        for (int i = 0; i < 18; ++i)
            lines[i] += tail[i];
        for (int i = 0; i < 9; ++i) {
            tail[i] = half[i] * win[i];
            tail[17 - i] = half[i] * win[17 - i];
        }
    }
    L3_change_sign(s->lines);
}
static void field_lines(Synthesis *s, const Fields *f) {
    for (int i = 0; i < 576; ++i) {
        int q = (int)f->q[i],
            idx = 420 + f->gain - 210 + 4 * BITS_DEQUANTIZER_OUT - (int)f->attenuation[i];
        float amp = idx >= 0 ? exponent[idx] : 0;
        s->lines[i] = q < 0 ? -power43[-q] * amp : power43[q] * amp;
    }
}
static void render_lines(Synthesis *s, int window, int ch, float *pcm) {
    hybrid(s, window, ch);
    mp3d_synth_granule(s->qmf[ch], s->lines, 18, 1, pcm, s->work);
}
void layer3_field_subbands(Synthesis *s, const Fields *f, int ch, float subbands[576]) {
    field_lines(s, f);
    hybrid(s, f->window, ch);
    memcpy(subbands, s->lines, sizeof(s->lines));
}
void layer3_render(Synthesis *s, Fields *f, int ch, float *pcm) {
    field_lines(s, f);
    render_lines(s, f->window, ch, pcm);
}
void layer3_render_spectrum(Synthesis *s, const Spectrum *spectrum, int ch, float *pcm) {
    memcpy(s->lines, spectrum->lines, sizeof(s->lines));
    render_lines(s, spectrum->window, ch, pcm);
}
int layer3_decode_bytes(Synthesis *s, const unsigned char *bytes, int length, float *pcm,
                        int *channels, int *rate) {
    if (length < 5 || layer3_frame_length(bytes) != length)
        return 0;
    mp3dec_frame_info_t info = {0};
    int n = mp3dec_decode_frame(&s->packed, bytes, length, pcm, &info);
    *channels = info.channels;
    *rate = info.hz;
    return n;
}

/* The MC bank represents left and right. Joint-stereo reconstruction happens in
   frequency space, before antialias, IMDCT, or synthesis. Use a legal scale per
   scalefactor band and keep the top implicit scale(s) at zero. */
static void joint_fields(Fields *f, const float *spectrum, const L3_gr_info_t *info) {
    double peak = 0;
    for (int j = 0; j < 576; j++)
        peak = fmax(peak, fabs(spectrum[j]));
    int unity = 210 - 4 * BITS_DEQUANTIZER_OUT;
    int gain = peak ? (int)ceil(unity + 4 * log2(peak) - (16. / 3.) * 11) : info->global_gain;
    f->gain = gain < 0 ? 0 : gain > 255 ? 255 : gain;
    int n = info->n_long_sfb + info->n_short_sfb;
    int coded = n - (info->n_short_sfb ? 3 : 1), j = 0;
    for (int k = 0; k < n; k++) {
        int end = j + info->sfbtab[k];
        double local = 0;
        for (int i = j; i < end; i++)
            local = fmax(local, fabs(spectrum[i]));
        int att = local && k < coded ? 2 * (int)fmin(7., floor(2 * log2(peak / local))) : 0;
        double inverse = exp2((unity - f->gain + att) * .25);
        for (; j < end; j++) {
            double q = fmin(8206., round(pow(fabs(spectrum[j]) * inverse, .75)));
            f->q[j] = copysign(q, spectrum[j]);
            f->attenuation[j] = att;
        }
    }
}

/* Unit probes describe the linear filter bank. They contain no user audio. */
void layer3_filter_basis(float bank[32][512], float analysis[18][36]) {
    for (int k = 0; k < 32; k++) {
        float qmf[15 * 64] = {0}, data[576] = {0}, work[33 * 64], impulse[576];
        data[18 * k] = 1;
        mp3d_synth_granule(qmf, data, 18, 1, impulse, work);
        memcpy(bank[k], impulse, 512 * sizeof(float));
    }
    for (int k = 0; k < 18; k++) {
        float data[576] = {0}, overlap[288] = {0};
        data[k] = 1;
        L3_imdct_gr(data, overlap, 0, 0);
        memcpy(analysis[k], data, 18 * sizeof(float));
        memset(data, 0, sizeof(data));
        L3_imdct_gr(data, overlap, 0, 0);
        memcpy(analysis[k] + 18, data, 18 * sizeof(float));
        for (int n = 0; n < 36; n++)
            analysis[k][n] /= 9.f;
    }
}
void layer3_long_fields(Fields *out, float lines[576], int hz) {
    /* Conjugating each alias-reduction rotation by a sign flip inverts it. */
    for (int pass = 0; pass < 2; pass++) {
        for (int band = 1; band < 32; band++)
            for (int k = 0; k < 8; k++)
                lines[18 * band + k] = -lines[18 * band + k];
        if (!pass)
            L3_antialias(lines, 31);
    }
    int version, frequency;
    if (!layer3_layout(hz, &version, &frequency)) {
        memset(out, 0, sizeof(*out));
        return;
    }
    int idx = layer3_band_index(version, frequency);
    L3_gr_info_t info = {0};
    info.sfbtab = g_scf_long[idx];
    info.n_long_sfb = 22;
    info.global_gain = 160;
    out->window = 0;
    joint_fields(out, lines, &info);
}
static int read_payload(Synthesis *s, const unsigned char *bytes, int length,
                        SpectralFrame *spectral, EncodedFrame *fields) {
    if (length < 5 || layer3_frame_length(bytes) != length)
        return 0;
    unsigned char h[4];
    memcpy(h, bytes, 4);
    if (HDR_GET_STEREO_MODE(h) != MODE_JOINT_STEREO)
        h[3] &= ~0x30;
    mp3dec_t *d = &s->packed;
    if (d->header[0] && !hdr_compare(d->header, h))
        memset(d, 0, sizeof(*d));
    memcpy(d->header, h, 4);
    mp3dec_scratch_t scratch = {0};
    bs_t bs;
    bs_init(&bs, bytes + 4, length - 4);
    if (HDR_IS_CRC(h))
        get_bits(&bs, 16);
    int back = L3_read_side_info(&bs, scratch.gr_info, h);
    if (back < 0 || bs.pos > bs.limit)
        goto invalid;
    int present = L3_restore_reservoir(d, &bs, &scratch, back);
    int channels = HDR_IS_MONO(h) ? 1 : 2, granules = HDR_TEST_MPEG1(h) ? 2 : 1;
    if (fields)
        field_header(fields, bytes, length);
    if (spectral) {
        spectral->channels = channels;
        spectral->granules = granules;
    }
    if (present && fields && !HDR_GET_STEREO_MODE_EXT(h)) {
        if (!independent_fields(fields, h, &scratch, &scratch.bs))
            goto invalid;
    } else if (present) {
        for (int gr = 0; gr < granules; gr++) {
            L3_gr_info_t *info = scratch.gr_info + gr * channels;
            memset(scratch.grbuf, 0, sizeof(scratch.grbuf));
            for (int c = 0; c < channels; c++) {
                int end = scratch.bs.pos + info[c].part_23_length;
                if (end > scratch.bs.limit)
                    goto invalid;
                L3_decode_scalefactors(h, scratch.ist_pos[c], &scratch.bs, info + c, scratch.scf,
                                       c);
                if (scratch.bs.pos > end)
                    goto invalid;
                L3_huffman(scratch.grbuf[c], &scratch.bs, info + c, scratch.scf, end);
            }
            if (HDR_TEST_I_STEREO(h))
                L3_intensity_stereo(scratch.grbuf[0], scratch.ist_pos[1], info, h);
            else if (HDR_IS_MS_STEREO(h))
                L3_midside_stereo(scratch.grbuf[0], 576);
            for (int c = 0; c < channels; c++) {
                int mixed = info[c].block_type == 2 && info[c].mixed_block_flag
                                ? (HDR_GET_MY_SAMPLE_RATE(h) == 2 ? 4 : 2)
                                : 0;
                int window = mixed ? (mixed == 4 ? 5 : 4) : info[c].block_type;
                int start = 18 * mixed;
                const uint8_t *short_bands = info[c].sfbtab + info[c].n_long_sfb;
                if (fields) {
                    Fields *f = &fields->fields[gr][c];
                    f->window = window;
                    joint_fields(f, scratch.grbuf[c], info + c);
                    if (info[c].n_short_sfb) {
                        L3_reorder(f->q + start, scratch.syn[0], short_bands);
                        L3_reorder(f->attenuation + start, scratch.syn[0], short_bands);
                    }
                } else {
                    if (info[c].n_short_sfb)
                        L3_reorder(scratch.grbuf[c] + start, scratch.syn[0], short_bands);
                    Spectrum *f = &spectral->gr[gr][c];
                    f->window = window;
                    memcpy(f->lines, scratch.grbuf[c], sizeof(f->lines));
                }
            }
        }
    }
    L3_save_reservoir(d, &scratch);
    return present;
invalid:
    memset(d, 0, sizeof(*d));
    return 0;
}
int layer3_read_fields(Synthesis *s, const unsigned char *h, int n, EncodedFrame *f) {
    return read_payload(s, h, n, NULL, f);
}
int layer3_read_spectrum(Synthesis *s, const unsigned char *h, int n, int hz, SpectralFrame *f) {
    return n >= 5 && layer3_header_rate(h) == hz && read_payload(s, h, n, f, NULL);
}
