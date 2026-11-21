#include "Codec.hpp"
#include <stdexcept>

namespace mpeg {
int nativeRate(int layer, double host, int divisor) {
    const int count = rateCount(layer);
    int hz = codec_rates[count - 1];
    for (int i = 0; i < count; i++)
        if (host >= codec_rates[i]) {
            hz = codec_rates[i];
            break;
        }
    for (int i = 0; i < count; i++) {
        double n = std::round(host / codec_rates[i]);
        if (n >= 1 && std::abs(host - n * codec_rates[i]) < 1e-6) {
            hz = codec_rates[i];
            break;
        }
    }
    return divided(format(layer, hz, 1), divisor);
}
int divided(Format f, int divisor) {
    int result = f.hz;
    for (int i = 0; i < rateCount(f.layer); i++) {
        int hz = codec_rates[i];
        if (hz <= f.hz && f.hz % hz == 0 &&
            std::max(f.original_hz, f.hz) / hz <= std::max(1, divisor))
            result = hz;
    }
    return result;
}
int byteTarget(int layer, int source, int ceiling, int divisor) {
    if (divisor <= 1 || source <= ceiling)
        return source;
    int target = source;
    for (int i = 0; i < rateCount(layer); i++) {
        int hz = codec_rates[i];
        if (hz <= source && source % hz == 0) {
            target = hz;
            if (target <= ceiling)
                break;
        }
    }
    return target;
}
int bitrate(Format f, int request) {
    int kb = request > 0 ? request : f.kbps;
    return f.layer == 3 ? pack_bitrate(f.hz, kb) : sb_bitrate(f.layer, f.hz, f.channels, kb, 1000);
}
void mixFrames(Frame &out, const Frame &live, const Frame *const sources[2][32], bool level) {
    out = live;
    int layer = live.fmt.layer, count = rows(layer, live.fmt.hz);
    for (int c = 0; c < live.fmt.channels; c++) {
        if (layer != 3) {
            for (int b = 0; b < 32; b++)
                if (const Frame *p = sources[c][b]) {
                    out.sub.allocation[c][b] = p->sub.allocation[c][b];
                    for (int j = 0; j < count; j++) {
                        out.sub.q[j][c][b] = p->sub.q[j][c][b];
                        if (level)
                            out.sub.scale[j / 12][c][b] = p->sub.scale[j / 12][c][b];
                    }
                }
            continue;
        }
        for (int g = 0; g < count / 18; g++) {
            Fields &dst = out.mp[g][c];
            int gain = 0;
            const Frame *first = sources[c][0];
            bool uniform = true;
            for (int b = 0; b < 32; b++) {
                const Frame *p = sources[c][b];
                uniform &= p == first;
                gain = std::max(gain, (p ? p : &live)->mp[g][c].gain);
            }
            if (level)
                dst.gain = gain;
            if (uniform && first && level)
                dst.window = first->mp[g][c].window;
            for (int b = 0; b < 32; b++) {
                const Fields &src = (sources[c][b] ? sources[c][b] : &live)->mp[g][c];
                for (int r = 0; r < 18; r++) {
                    int j = 18 * b + r;
                    dst.q[j] = src.q[j];
                    if (level)
                        field_rebase(&dst, &src, j, src.gain);
                    else
                        dst.attenuation[j] = src.attenuation[j];
                }
            }
        }
    }
}
// The byte decoder keeps the unquantized joint-stereo spectrum. Parsing every
// incoming frame maintains the bit reservoir even while the audible frame is held.
struct Codec::BytePlayback {
    SpectralFrame incoming{}, held{};
    MPEGRate decimator{};
    int sourceHz = 0, targetHz = 0;
    bool have = false;
};
bool Codec::decodeMp3Bytes(const Packet *packet, const Options &opt, const Config &cfg,
                           Buffer &out) {
    if (!bytePlayback)
        bytePlayback = std::make_unique<BytePlayback>();
    auto &b = *bytePlayback;
    if (packet) {
        int hz = layer3_header_rate(packet->data);
        if (hz != b.sourceHz) {
            b.have = false;
            b.targetHz = 0;
            b.sourceHz = hz;
            layer3_clear(parser);
            layer3_clear(synthesis);
        }
        if (!layer3_read_spectrum(parser, packet->data, packet->size, hz, &b.incoming))
            return false;
    } else if (!b.have)
        return false;
    auto &held = b.held;
    const auto &in = b.incoming;
    held.channels = cfg.channels;
    held.granules = in.granules;
    if (!b.have || !opt.freeze) {
        for (int g = 0; g < held.granules; g++)
            for (int c = 0; c < cfg.channels; c++) {
                auto &to = held.gr[g][c];
                const auto &from = in.gr[g][std::min(c, in.channels - 1)];
                if (packet)
                    to.window = from.window;
                for (int k = 0; k < 32; k++)
                    if (!b.have || !(opt.masks[c] & (1u << k))) {
                        if (packet)
                            std::copy_n(from.lines + 18 * k, 18, to.lines + 18 * k);
                        else
                            std::fill_n(to.lines + 18 * k, 18, 0.f);
                    }
            }
    }
    b.have = true;
    for (int g = 0; g < held.granules; g++)
        for (int c = 0; c < cfg.channels; c++)
            layer3_render_spectrum(synthesis, &held.gr[g][c], c, out.audio.v[c] + 576 * g);
    int count = held.granules * 576;
    int target = byteTarget(3, b.sourceHz, nativeRate(3, cfg.host, opt.divisor), opt.divisor);
    if (target != b.targetHz) {
        rate_setup_native(&b.decimator, b.sourceHz, target);
        b.targetHz = target;
    }
    if (target != b.sourceHz) {
        int n = 0;
        for (int i = 0; i < count; i++) {
            rate_push(&b.decimator, out.audio.v[0][i], out.audio.v[cfg.channels - 1][i]);
            if (codec_ticks(&b.decimator.clock)) {
                for (int c = 0; c < cfg.channels; c++)
                    out.audio.v[c][n] = rate_filter(&b.decimator, c);
                ++n;
            }
        }
        count = n;
    }
    if (cfg.channels == 1)
        std::copy_n(out.audio.v[0], count, out.audio.v[1]);
    out.frame.fmt = format(3, target, cfg.channels);
    out.frame.fmt.original_hz = b.sourceHz;
    out.duration = count;
    out.hasAudio = true;
    out.bytes.size = 0;
    return true;
}
Codec::Codec(int l) : layer(l) {
    if (l == 3) {
        parser = layer3_new();
        synthesis = layer3_new();
        if (!parser || !synthesis) {
            layer3_free(parser);
            layer3_free(synthesis);
            throw std::bad_alloc();
        }
    }
}
Codec::~Codec() {
    if (lame)
        lame_close(lame);
    spectral_rate_free(mpConvert);
    layer3_free(parser);
    layer3_free(synthesis);
}
void Codec::resetStream() {
    padding = {};
    mpPack = {};
    renderFormat = {};
    std::memset(subSynth, 0, sizeof subSynth);
    if (parser)
        layer3_clear(parser);
    if (synthesis)
        layer3_clear(synthesis);
    resetConversion();
}
void Codec::resetConversion() {
    spectral_rate_free(mpConvert);
    mpConvert = nullptr;
    conversion = {};
    dest = 0;
    inGranules = outFrames = 0;
}
static Format mpFormat(const EncodedFrame &p, int original) {
    auto f = format(3, p.rate, p.channels);
    f.original_hz = original ? original : p.rate;
    f.kbps = p.kbps;
    if (p.length >= 4) {
        f.kbps = br[p.rate >= 32000][p.bytes[2] >> 4];
        f.mode = p.bytes[3] >> 6;
        f.pad = (p.bytes[2] >> 1) & 1;
    }
    return f;
}
bool Codec::unpack(const Packet &bytes, Frame &f) {
    if (layer != 3) {
        if (!sb_unpack(bytes.data, bytes.size, &subPacket) || subPacket.fields.fmt.layer != layer)
            return false;
        f.sub = subPacket.fields;
        f.fmt = f.sub.fmt;
        return true;
    }
    if (!layer3_read_fields(parser, bytes.data, bytes.size, &mpPacket))
        return false;
    f.fmt = mpFormat(mpPacket, 0);
    std::memcpy(f.mp, mpPacket.fields, sizeof f.mp);
    return true;
}
bool Codec::pack(const Frame &f, int request, Buffer &out, bool preserve) {
    Format fmt = f.fmt;
    int kb = bitrate(fmt, request);
    if (layer != 3) {
        if (!sb_pack(&f.sub, kb, preserve, &padding, &subPacket))
            return false;
        out.frame.sub = subPacket.fields;
        out.frame.fmt = subPacket.fields.fmt;
        out.bytes.size = subPacket.length;
        std::memcpy(out.bytes.data, subPacket.data, out.bytes.size);
        return true;
    }
    if (mpPack.rate != fmt.hz || mpPack.channels != fmt.channels) {
        if (!pack_init(&mpPack, fmt.hz, fmt.channels, kb))
            return false;
    }
    mpPack.bitrate = kb;
    out.bytes.size = preserve ? pack_file_frame(&mpPack, f.mp, out.bytes.data)
                              : pack_frame(&mpPack, f.mp, out.bytes.data);
    if (!out.bytes.size || !layer3_unpack(&mpPacket, out.bytes.data, out.bytes.size))
        return false;
    out.frame.fmt = mpFormat(mpPacket, fmt.original_hz);
    std::memcpy(out.frame.mp, mpPacket.fields, sizeof f.mp);
    return true;
}
bool Codec::encode(const Audio &audio, Format f, Buffer &out) {
    int samples = rows(layer, f.hz) * 32;
    if (f.hz != encoderFormat.hz || f.channels != encoderFormat.channels) {
        std::memset(analysis, 0, sizeof analysis);
        pendingCount = 0;
        if (lame) {
            lame_close(lame);
            lame = nullptr;
        }
        encoderFormat = f;
    }
    if (layer != 3) {
        for (int r = 0; r < samples / 32; r++)
            for (int c = 0; c < f.channels; c++)
                filter_analyze(analysis + c, audio.v[c] + 32 * r, bands.x[r][c]);
        Frame raw;
        raw.fmt = f;
        sb_quantize(&bands, f, &raw.sub);
        return pack(raw, f.kbps, out, false);
    }
    if (!lame) {
        lame = lame_init();
        if (!lame)
            throw std::bad_alloc();
        lame_set_num_channels(lame, f.channels);
        lame_set_mode(lame, f.channels == 1 ? MONO : STEREO);
        lame_set_in_samplerate(lame, f.hz);
        lame_set_out_samplerate(lame, f.hz);
        lame_set_brate(lame, f.kbps);
        lame_set_VBR(lame, vbr_off);
        lame_set_quality(lame, 3);
        lame_set_disable_reservoir(lame, 1);
        lame_set_bWriteVbrTag(lame, 0);
        lame_set_write_id3tag_automatic(lame, 0);
        if (lame_init_params(lame) < 0)
            throw std::runtime_error("LAME configuration failed");
        lameKbps = f.kbps;
    }
    if (lameKbps != f.kbps) {
        if (lame_set_frame_bitrate(lame, f.kbps) < 0)
            throw std::runtime_error("LAME rejected bitrate");
        lameKbps = f.kbps;
    }
    int n = lame_encode_buffer_ieee_float(lame, audio.v[0], audio.v[f.channels - 1], samples,
                                          pending + pendingCount, sizeof pending - pendingCount);
    if (n < 0)
        throw std::runtime_error("LAME failed to encode frame");
    pendingCount += n;
    if (pendingCount < 4)
        return false;
    int len = layer3_frame_length(pending);
    if (!len)
        throw std::runtime_error("invalid generated MP3 header");
    if (len > pendingCount)
        return false;
    out.bytes.size = len;
    std::memcpy(out.bytes.data, pending, len);
    pendingCount -= len;
    std::memmove(pending, pending + len, pendingCount);
    if (!layer3_unpack(&mpPacket, out.bytes.data, len))
        throw std::runtime_error("invalid generated MP3 payload");
    out.frame.fmt = mpFormat(mpPacket, f.original_hz);
    std::memcpy(out.frame.mp, mpPacket.fields, sizeof out.frame.mp);
    return true;
}
void Codec::synth(const Frame &f, Audio &out) {
    if (f.fmt.hz != renderFormat.hz || f.fmt.channels != renderFormat.channels) {
        renderFormat = f.fmt;
        std::memset(subSynth, 0, sizeof subSynth);
        if (synthesis)
            layer3_clear(synthesis);
    }
    int count = rows(layer, f.fmt.hz);
    for (int c = 0; c < f.fmt.channels; c++) {
        if (layer == 3) {
            for (int g = 0; g < count / 18; g++) {
                Fields fields = f.mp[g][c];
                layer3_render(synthesis, &fields, c, out.v[c] + 576 * g);
            }
        } else
            for (int r = 0; r < count; r++) {
                float band[32];
                for (int b = 0; b < 32; b++)
                    band[b] = sb_amplitude(&f.sub, r, c, b);
                filter_synthesize(subSynth + c, band, out.v[c] + 32 * r);
            }
    }
    if (f.fmt.channels == 1)
        std::memcpy(out.v[1], out.v[0], count * 32 * sizeof(float));
}
bool Codec::convert(const Frame &f, int target, Frame &out) {
    if (conversion.hz != f.fmt.hz || conversion.channels != f.fmt.channels || dest != target) {
        resetConversion();
        conversion = f.fmt;
        dest = target;
        if (target != f.fmt.hz) {
            if (layer == 3) {
                mpConvert = spectral_rate_new(f.fmt.hz, target, f.fmt.channels);
                if (!mpConvert)
                    throw std::runtime_error("spectral conversion unavailable");
            } else
                convert_start(&subConvert, f.fmt, target);
        }
    }
    if (target == f.fmt.hz) {
        out = f;
        return true;
    }
    if (layer != 3) {
        if (!convert_frame(&subConvert, &f.sub, &out.sub))
            return false;
        out.fmt = out.sub.fmt;
        return true;
    }
    bool ready = false;
    int factor = f.fmt.hz / target;
    for (int g = 0; g < rows(layer, f.fmt.hz) / 18; g++) {
        if (inGranules % factor == 0)
            conversionKbps[(inGranules / factor) % 16] = f.fmt.kbps;
        inGranules++;
        if (spectral_rate_fields(mpConvert, f.mp[g], out.mp[0]))
            ready = true;
    }
    if (ready) {
        out.fmt = format(3, target, f.fmt.channels);
        out.fmt.original_hz = f.fmt.original_hz;
        out.fmt.kbps = conversionKbps[outFrames++ % 16];
    }
    return ready;
}
bool Codec::finish(Frame &out) {
    if (!dest || dest == conversion.hz)
        return false;
    if (layer != 3) {
        if (!convert_finish(&subConvert, &out.sub))
            return false;
        out.fmt = out.sub.fmt;
        return true;
    }
    if (!spectral_rate_finish(mpConvert, out.mp[0]))
        return false;
    out.fmt = format(3, dest, conversion.channels);
    out.fmt.original_hz = conversion.original_hz;
    out.fmt.kbps = conversionKbps[outFrames++ % 16];
    return true;
}
void FileSource::close() {
    reader_close(sub);
    field_file_close(mp);
    sub = nullptr;
    mp = nullptr;
}
bool FileSource::open(const char *path, int l, int divisor) {
    close();
    layer = l;
    if (l == 3) {
        mp = field_file_open_at(path, divisor, &error);
        if (mp)
            original = field_file_source_rate(mp);
        return mp;
    }
    sub = reader_open(path, l, divisor, &error);
    return sub;
}
int FileSource::next(Buffer &out) {
    int terminal = 0;
    if (layer == 3) {
        const EncodedFrame *p = field_file_peek(mp, &terminal);
        if (p) {
            out.frame.fmt = mpFormat(*p, original);
            std::memcpy(out.frame.mp, p->fields, sizeof out.frame.mp);
            out.bytes.size = p->length;
            if (p->length)
                std::memcpy(out.bytes.data, p->bytes, p->length);
            field_file_advance(mp);
            return 1;
        }
        if (terminal < 0)
            error = field_file_error(mp);
    } else {
        const BandFrame *p = reader_peek(sub, &terminal);
        if (p) {
            out.frame.fmt = p->fmt;
            out.frame.sub = *p;
            out.bytes.size = 0;
            reader_advance(sub);
            return 1;
        }
        if (terminal < 0)
            error = reader_error(sub);
    }
    return terminal < 0 ? -1 : terminal ? 2 : 0;
}
} // namespace mpeg
