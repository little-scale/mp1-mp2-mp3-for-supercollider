#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
extern "C" {
#include "codec/layer3.h"
#include "codec/pack.h"
#include "codec/field_bus.h"
#include "codec/bitrate.h"
#include "codec/spectral_rate.h"
#include "codec/field_file.h"
#include "codec/vendor/lame.h"
#define Rate MPEGRate
#include "codec/rate.h"
#undef Rate
#include "codec/layers12/subband.h"
#define Synthesis SubbandSynthesis
#include "codec/layers12/convert.h"
#undef Synthesis
#include "codec/layers12/reader.h"
#include "codec/layers12/layer1.h"
}

namespace mpeg {
enum class Role { Encode, Decode, Pack, Unpack, Frames, Clock, Short, Bands, Serial };
constexpr int byteLanes = 8, maxBytes = 2048, maxRows = 36;
inline int limit(double x, int low, int high) {
    return std::isfinite(x)
               ? static_cast<int>(std::round(std::max<double>(low, std::min<double>(x, high))))
               : std::clamp(0, low, high);
}
inline int rows(int layer, int hz) {
    return layer == 3 ? (hz >= 32000 ? 36 : 18) : sb_rows(layer);
}
inline int rateCount(int layer) {
    return layer == 3 ? 9 : 6;
}
inline bool validRate(int layer, int hz) {
    return std::find(codec_rates, codec_rates + rateCount(layer), hz) !=
           codec_rates + rateCount(layer);
}
int nativeRate(int layer, double host, int divisor);
int divided(Format f, int divisor);
int bitrate(Format f, int requested);
int byteTarget(int layer, int source, int ceiling, int divisor);
inline Format format(int layer, int hz, int channels) {
    return {layer, hz, 0, channels, channels == 1 ? 3 : 0, 32, -1, hz};
}
struct Frame {
    Format fmt{};
    union {
        BandFrame sub;
        Fields mp[2][2];
    };
    Frame() : mp{} {}
};
struct Packet {
    int size = 0;
    unsigned char data[maxBytes]{};
};
struct Audio {
    float v[2][1152]{};
};
struct Buffer {
    Frame frame;
    Packet bytes;
    Audio audio;
    bool hasAudio = false;
    int duration = 0;
    unsigned generation = 0;
};
struct Options {
    int kbps = 0, divisor = 1;
    bool freeze = false, level = false, monitor = false, normalized = false;
    unsigned masks[2]{};
};
struct Config {
    Role role;
    int layer, channels, mode, capacity, binputs;
    double host;
    char path[1024], restricted[1024];
};
void mixFrames(Frame &out, const Frame &live, const Frame *const sources[2][32], bool level);
class Codec {
  public:
    Codec(int layer);
    ~Codec();
    bool unpack(const Packet &, Frame &);
    bool decodeMp3Bytes(const Packet *, const Options &, const Config &, Buffer &);
    bool pack(const Frame &, int request, Buffer &, bool preserve);
    bool encode(const Audio &, Format f, Buffer &);
    void synth(const Frame &, Audio &);
    bool convert(const Frame &, int target, Frame &);
    bool finish(Frame &);
    void resetConversion();
    void resetStream();

  private:
    struct BytePlayback;
    std::unique_ptr<BytePlayback> bytePlayback;
    int layer;
    Analysis analysis[2]{};
    SubbandSynthesis subSynth[2]{};
    ::Synthesis *parser = nullptr, *synthesis = nullptr;
    ::Packet subPacket{};
    EncodedFrame mpPacket{};
    Padding padding{};
    Pack mpPack{};
    BandAudio bands{};
    Converter subConvert{};
    SpectralRate *mpConvert = nullptr;
    Format conversion{}, renderFormat{}, encoderFormat{};
    int dest = 0;
    unsigned long inGranules = 0, outFrames = 0;
    int conversionKbps[16]{};
    lame_t lame = nullptr;
    int lameKbps = 0, pendingCount = 0;
    unsigned char pending[16384]{};
};
class FileSource {
  public:
    ~FileSource() {
        close();
    }
    bool open(const char *path, int layer, int divisor);
    int next(Buffer &); // 1 frame, 0 temporary underrun, 2 end, -1 error
    void close();
    const char *error = "file error";

  private:
    Reader *sub = nullptr;
    FieldFile *mp = nullptr;
    int layer = 0, original = 0;
};
} // namespace mpeg
