#include "SC_PlugIn.h"
#include "Service.hpp"
#include <new>
#include <filesystem>
#include <stdexcept>

static InterfaceTable *ft;
namespace {
using namespace mpeg;
using Frame = mpeg::Frame;
using Packet = mpeg::Packet;
enum In {
    Channels,
    Capacity,
    Mode,
    ByteInputs,
    Kbps,
    Divisor,
    Freeze,
    HoldMode,
    Normalized,
    Run,
    Record,
    Clear,
    Monitor,
    PathLength,
    Header
};
inline float get(Unit *u, int port, int sample) {
    return u->mInBuf[port][u->mInput[port]->mCalcRate == calc_FullRate ? sample : 0];
}
int fieldWidth(int l, int c) {
    return (l == 3 ? 66 : 96) * c + 8;
}
struct Path {
    MPEGRate rate{};
    Fractional fractional{};
};
class RateBank {
  public:
    Path paths[9]{};
    int active = -1, count = 0;
    void prepare(int layer, double host, bool input, bool audio = true) {
        count = rateCount(layer);
        for (int i = 0; i < count; i++) {
            int hz = codec_rates[i];
            auto &p = paths[i];
            if (!audio) {
                p.rate.hz = p.rate.clock.hz = hz;
                p.rate.clock.host = host;
                p.rate.factor = 1;
                continue;
            }
            if (!rate_setup_native(&p.rate, host, hz) ||
                (p.rate.factor < 0 &&
                 !fractional_setup(&p.fractional, input ? host : hz, input ? hz : host)))
                throw std::runtime_error("rate conversion allocation failed");
        }
    }
    ~RateBank() {
        for (auto &p : paths)
            fractional_free(&p.fractional);
    }
    Path &select(int hz) {
        if (active < 0 || paths[active].rate.hz != hz) {
            active = 0;
            while (active + 1 < count && paths[active].rate.hz != hz)
                ++active;
            Path &p = paths[active];
            p.rate.clock.first = 1;
            p.rate.clock.phase = 0;
            p.rate.head = 0;
            std::memset(p.rate.delay, 0, sizeof p.rate.delay);
            p.fractional.head = 0;
            if (p.fractional.history)
                std::memset(p.fractional.history, 0, 2 * p.fractional.taps * sizeof(float));
        }
        return paths[active];
    }
};
struct Front {
    Config cfg;
    int base, width, bandCount;
    bool offline;
    std::unique_ptr<Service> service;
    std::unique_ptr<Frame[]> memory;
    int head = 0, used = 0;
    bool storeFrame = false;
    const Frame *chosen[2][32]{};
    RateBank inRate, outRate;
    Job job, controlJob;
    Buffer current, memoryOut;
    bool active = false, available = false, started = false;
    int preroll = 0, idleSamples = 0, nativeHz = 0, currentHz = 0;
    unsigned gen = 1, byteRead = 0, byteWrite = 0, localDrops = 0;
    uint64_t take = 0, captureTake = 0;
    unsigned char fifo[8192]{};
    float latched[200]{};
    Format activeFmt{};
    int pos = 0, next = -1, pcmPos = 0, inputHz = 0, internalPos = 0, lastDiv = 0, clockRow = 0;
    int usedBytes = 0, needBytes = 0, quiet = 0, idleIntervals = 0, grace = 0, partialTimeout = 0;
    double phase = 0;
    bool firstTick = true, previousRun = false, previousClear = false, clearPending = false,
         idleDrained = true, retryReader = false;
    Layer1 serial{};
    double serialWords[32]{}, serialAlloc[32]{}, serialScale[32]{};
    uint32_t written = 0;
    explicit Front(const Config &c, bool nrt)
        : cfg(c), base(Header + std::strlen(c.path)), width(fieldWidth(c.layer, c.channels)),
          bandCount(32 * c.channels), offline(nrt) {
        nativeHz = nativeRate(c.layer, c.host, 1);
        currentHz = nativeHz;
        if (c.path[0] && c.restricted[0]) {
            namespace fs = std::filesystem;
            auto root = fs::canonical(c.restricted);
            auto path = fs::weakly_canonical(fs::absolute(c.path));
            auto relative = path.lexically_relative(root);
            if (relative.empty() || *relative.begin() == "..")
                throw std::runtime_error("file is outside the server's restricted path");
        }
        if (c.role == Role::Encode)
            inRate.prepare(c.layer, c.host, true);
        if (c.role == Role::Decode || c.role == Role::Pack || c.role == Role::Encode ||
            c.role == Role::Frames || c.role == Role::Unpack)
            outRate.prepare(c.layer, c.host, false, c.role == Role::Decode || c.role == Role::Pack);
        if (c.role == Role::Frames)
            memory = std::make_unique<Frame[]>(c.capacity);
        if (c.role == Role::Encode || c.role == Role::Decode || c.role == Role::Pack ||
            c.role == Role::Unpack)
            service = std::make_unique<Service>(c, nrt);
        layer1_reset(&serial);
    }
    int divisor(Unit *u, int i) {
        return limit(get(u, Divisor, i), 1, 6);
    }
    Options controls(Unit *u, int i, int gate = -1) {
        Options o;
        o.kbps = limit(get(u, Kbps, i), 0, 448);
        o.divisor = divisor(u, i);
        o.freeze = get(u, Freeze, i) >= .5f;
        o.level = get(u, HoldMode, i) >= .5f;
        o.normalized = get(u, Normalized, i) >= .5f;
        o.monitor = get(u, Monitor, i) >= .5f;
        if (gate >= 0)
            for (int c = 0; c < cfg.channels; c++)
                for (int b = 0; b < 32; b++)
                    if (get(u, gate + 32 * c + b, i) >= .5f)
                        o.masks[c] |= 1u << b;
        return o;
    }
    void command(Action a, const Options &o = {}) {
        if (!service)
            return;
        controlJob.action = a;
        controlJob.generation = gen;
        controlJob.opt = o;
        controlJob.take = take;
        bool queued = service->post(controlJob);
        if (cfg.role == Role::Unpack)
            retryReader = !queued;
    }
    int internal(int hz) {
        int ticks;
        if (firstTick) {
            ticks = 1;
            firstTick = false;
        } else {
            phase += hz;
            ticks = static_cast<int>(phase / cfg.host);
            phase -= ticks * cfg.host;
        }
        int marker = 0, period = 32 * rows(cfg.layer, hz);
        while (ticks--) {
            if (internalPos % 32 == 0)
                marker = 1 + internalPos / 32;
            if (++internalPos >= period)
                internalPos = 0;
        }
        return marker;
    }
    Format metadata(Unit *u, int i, int hz) {
        int m = base + width - 7, source = limit(get(u, m, i), 0, 48000);
        auto f = format(cfg.layer, hz, cfg.channels);
        f.original_hz = nativeHz;
        if (!validRate(cfg.layer, source))
            return f;
        f.hz = source;
        f.kbps = limit(get(u, m + 1, i), 0, 448);
        f.channels = limit(get(u, m + 2, i), 1, cfg.channels);
        f.mode = f.channels == 1 ? 3 : limit(get(u, m + 3, i), 0, 2);
        f.bound = f.mode == 1 ? 4 * limit(get(u, m + 4, i) / 4, 1, 4) : 32;
        f.pad = limit(get(u, m + 5, i), -1, 1);
        int original = limit(get(u, m + 6, i), 0, 48000);
        f.original_hz =
            validRate(cfg.layer, original) && original >= source && original % source == 0
                ? original
                : source;
        return f;
    }
    void begin(Unit *u, int i, Format fmt) {
        Frame &f = job.data.frame;
        if (f.fmt.hz != fmt.hz || f.fmt.channels != fmt.channels) {
            used = head = 0;
        }
        f.fmt = fmt;
        if (cfg.layer != 3)
            f.sub.fmt = fmt;
        next = 0;
        captureTake = previousRun ? take : 0;
        job.opt = controls(u, i, cfg.role == Role::Frames ? -1 : base + width);
        if (cfg.role == Role::Frames) {
            if (clearPending) {
                head = used = 0;
                clearPending = false;
            }
            storeFrame = get(u, Record, i) >= .5f;
            for (int c = 0; c < fmt.channels; c++)
                for (int b = 0; b < 32; b++) {
                    int age = limit(get(u, base + width + 32 * c + b, i), 0, used);
                    chosen[c][b] =
                        age ? &memory[(head + cfg.capacity - age) % cfg.capacity] : nullptr;
                }
        }
    }
    void capture(Unit *u, int i, int r) {
        Frame &f = job.data.frame;
        int n = bandCount;
        for (int c = 0; c < f.fmt.channels; c++) {
            if (cfg.layer == 3) {
                Fields &g = f.mp[r / 18][c];
                if (r % 18 == 0) {
                    g.gain = limit(get(u, base + 2 * n + c, i), 0, 255);
                    g.window = limit(get(u, base + 2 * n + cfg.channels + c, i), 0, 5);
                }
                for (int b = 0; b < 32; b++) {
                    int k = c * 32 + b, j = b * 18 + r % 18;
                    g.q[j] =
                        limit(get(u, base + k, i) * (job.opt.normalized ? 8206. : 1.), -8206, 8206);
                    g.attenuation[j] = limit(get(u, base + n + k, i), 0, 210);
                }
            } else
                for (int b = 0; b < 32; b++) {
                    int k = c * 32 + b;
                    if (!r)
                        f.sub.allocation[c][b] =
                            limit(get(u, base + n + k, i), 0, cfg.layer == 1 ? 14 : 17);
                    if (r % 12 == 0)
                        f.sub.scale[r / 12][c][b] = limit(get(u, base + 2 * n + k, i), 0, 62);
                    int center = sb_levels(cfg.layer, f.sub.allocation[c][b]) / 2;
                    f.sub.q[r][c][b] = limit(
                        get(u, base + k, i) * (job.opt.normalized ? center : 1.), -center, center);
                }
        }
        if (++next != rows(cfg.layer, f.fmt.hz))
            return;
        next = -1;
        quiet = idleIntervals = 0;
        idleDrained = false;
        grace = static_cast<int>(std::ceil(cfg.host * 32 * rows(cfg.layer, f.fmt.hz) / f.fmt.hz));
        if (cfg.role == Role::Frames) {
            mixFrames(memoryOut.frame, f, chosen, true);
            available = true;
            if (storeFrame) {
                memory[head] = f;
                head = (head + 1) % cfg.capacity;
                used = std::min(used + 1, cfg.capacity);
            }
        } else {
            job.action = Action::Frame;
            job.generation = gen;
            job.take = take;
            job.record = captureTake && captureTake == take && previousRun;
            service->post(job);
        }
    }
    void idle(Unit *u, int i, int gate) {
        if (!grace || ++quiet < grace)
            return;
        quiet = 0;
        idleIntervals = std::min(2, idleIntervals + 1);
        Options o = controls(u, i, gate);
        if (cfg.role == Role::Decode && (o.freeze || o.masks[0] || o.masks[1])) {
            command(Action::Repeat, o);
            idleDrained = false;
        } else if (!idleDrained && idleIntervals >= 2) {
            command(Action::Drain, o);
            idleDrained = true;
        }
    }
    void inputFields(Unit *u, int i, int hz) {
        float v = get(u, base + width - 8, i);
        int mark = v == -2 ? internal(hz)
                           : (std::isfinite(v) && v == std::floor(v) && v >= -1 && v <= 36
                                  ? static_cast<int>(v)
                                  : 0);
        if (cfg.role == Role::Frames) {
            bool clear = get(u, Clear, i) > 0;
            if (clear && !previousClear)
                clearPending = true;
            previousClear = clear;
        }
        if (mark == 1)
            begin(u, i, metadata(u, i, hz));
        if (mark > 0 && mark - 1 == next)
            capture(u, i, mark - 1);
        if (mark < 0) {
            next = -1;
            if (cfg.role != Role::Frames)
                idle(u, i, base + width);
        } else if (mark != rows(cfg.layer, job.data.frame.fmt.hz) && quiet < grace)
            ++quiet;
    }
    void inputBytes(Unit *u, int i) {
        bool arrived = false;
        auto &packet = job.data.bytes;
        for (int lane = 0; lane < cfg.binputs; lane++) {
            double value = get(u, base + lane, i);
            if (!std::isfinite(value) || value < 0 || value > 255)
                continue;
            arrived = true;
            packet.data[usedBytes++] = static_cast<unsigned char>(value + .5);
            if (usedBytes < 4)
                continue;
            if (!needBytes) {
                Format f{};
                needBytes =
                    cfg.layer == 3 ? layer3_frame_length(packet.data) : sb_header(packet.data, &f);
                if (needBytes <= 4 || needBytes > maxBytes ||
                    (cfg.layer != 3 && f.layer != cfg.layer)) {
                    std::memmove(packet.data, packet.data + 1, 3);
                    usedBytes = 3;
                    needBytes = 0;
                    continue;
                }
            }
            if (usedBytes != needBytes)
                continue;
            packet.size = needBytes;
            int hz;
            if (cfg.layer == 3)
                hz = layer3_header_rate(packet.data);
            else {
                Format f;
                sb_header(packet.data, &f);
                hz = f.hz;
            }
            job.action = Action::Bytes;
            job.generation = gen;
            job.opt = controls(u, i, base + cfg.binputs);
            service->post(job);
            usedBytes = needBytes = quiet = idleIntervals = 0;
            grace = static_cast<int>(std::ceil(cfg.host * 32 * rows(cfg.layer, hz) / hz));
            idleDrained = false;
        }
        if (arrived)
            partialTimeout = std::max(2048, 2 * grace);
        else if (partialTimeout > 0)
            --partialTimeout;
        else
            usedBytes = needBytes = 0;
        if (!usedBytes)
            idle(u, i, base + cfg.binputs);
        else if (quiet < grace)
            ++quiet;
    }
    void inputAudio(Unit *u, int i, int hz) {
        if (inputHz != hz) {
            inputHz = hz;
            pcmPos = 0;
            ++gen;
        }
        Path &p = inRate.select(hz);
        MPEGRate &r = p.rate;
        float x[2];
        for (int c = 0; c < 2; c++) {
            float v = get(u, base + std::min(cfg.channels - 1, c), i);
            x[c] = std::isfinite(v) ? std::clamp(v, -1.f, 1.f) : 0.f;
        }
        if (r.factor > 1)
            rate_push(&r, x[0], x[1]);
        if (r.factor < 0)
            fractional_push(&p.fractional, x[0], x[1]);
        int ticks = codec_ticks(&r.clock);
        for (int k = 0; k < ticks; k++) {
            if (!pcmPos) {
                job.opt = controls(u, i);
                job.data.frame.fmt = format(cfg.layer, hz, cfg.channels);
                auto &f = job.data.frame.fmt;
                f.original_hz = nativeHz;
                f.kbps = (cfg.layer == 3 && !job.opt.kbps) ? (hz >= 32000   ? 128
                                                              : hz >= 16000 ? 64
                                                                            : 32)
                                                           : bitrate(f, job.opt.kbps);
            }
            for (int c = 0; c < cfg.channels; c++)
                job.data.audio.v[c][pcmPos] =
                    r.factor == 1 ? x[c]
                    : r.factor > 1
                        ? rate_filter(&r, c)
                        : fractional_read(&p.fractional, c,
                                          ((ticks - k - 1) * cfg.host + r.clock.phase) / hz);
            if (++pcmPos == 32 * rows(cfg.layer, hz)) {
                job.action = Action::Frame;
                job.generation = gen;
                service->post(job);
                pcmPos = 0;
            }
        }
    }
    bool receive() {
        bool ok = false;
        if (cfg.role == Role::Frames) {
            if (available) {
                current = memoryOut;
                available = false;
                ok = true;
            }
        } else
            while (service->take(current)) {
                if (current.generation == gen) {
                    ok = true;
                    break;
                }
            }
        if (!ok)
            return false;
        active = true;
        pos = 0;
        activeFmt = current.frame.fmt;
        if (!started) {
            int n = current.duration ? current.duration : 32 * rows(cfg.layer, activeFmt.hz);
            int buffers = cfg.role == Role::Frames || cfg.role == Role::Unpack ? 0
                          : !offline                                           ? 2
                                     : (cfg.role == Role::Decode && cfg.mode ? 1 : 0);
            preroll = static_cast<int>(std::ceil(n * cfg.host / activeFmt.hz)) * buffers;
            started = true;
        }
        idleSamples = 0;
        return true;
    }
    void sendBytes() {
        const Packet &p = current.bytes;
        if (cfg.role != Role::Decode && cfg.role != Role::Frames && p.size > 0) {
            if (byteWrite - byteRead + static_cast<unsigned>(p.size) <= sizeof fifo) {
                for (int k = 0; k < p.size; k++)
                    fifo[byteWrite++ % sizeof fifo] = p.data[k];
            } else
                ++localDrops;
        }
    }
    void latch(int r) {
        const Frame &f = current.frame;
        int n = bandCount;
        for (int c = 0; c < cfg.channels; c++) {
            int cc = std::min(c, f.fmt.channels - 1);
            for (int b = 0; b < 32; b++) {
                int k = 32 * c + b;
                if (cfg.layer == 3) {
                    auto &g = f.mp[r / 18][cc];
                    int j = b * 18 + r % 18;
                    latched[k] = g.q[j];
                    latched[n + k] = g.attenuation[j];
                } else {
                    latched[k] = f.sub.q[r][cc][b];
                    latched[n + k] = f.sub.allocation[cc][b];
                    latched[2 * n + k] = f.sub.scale[r / 12][cc][b];
                }
            }
            if (cfg.layer == 3) {
                latched[2 * n + c] = f.mp[r / 18][cc].gain;
                latched[2 * n + cfg.channels + c] = f.mp[r / 18][cc].window;
            }
        }
        float meta[] = {float(f.fmt.hz),         float(f.fmt.kbps),  float(f.fmt.channels),
                        float(f.fmt.mode),       float(f.fmt.bound), float(f.fmt.pad),
                        float(f.fmt.original_hz)};
        std::copy(meta, meta + 7, latched + width - 7);
    }
    void output(Unit *u, int i, int fallback) {
        if (!active)
            receive();
        int hz = activeFmt.hz ? activeFmt.hz : fallback;
        Path &p = outRate.select(hz);
        MPEGRate &r = p.rate;
        int ticks = codec_ticks(&r.clock);
        bool playing = active && preroll == 0;
        if (preroll > 0)
            --preroll;
        if (!active && started &&
            ++idleSamples > std::ceil(2 * cfg.host * 32 * rows(cfg.layer, hz) / hz))
            started = false;
        int mark = playing ? 0 : -1;
        float sample[2]{};
        bool sounding = playing;
        for (int k = 0; k < ticks; k++) {
            if (k && !active) {
                playing = receive();
                if (playing && activeFmt.hz != hz)
                    break;
            }
            sample[0] = sample[1] = 0;
            if (playing && active) {
                sounding = true;
                if (!pos)
                    sendBytes();
                if (pos % 32 == 0) {
                    mark = pos / 32 + 1;
                    if (cfg.role != Role::Decode)
                        latch(pos / 32);
                }
                if (current.hasAudio) {
                    sample[0] = current.audio.v[0][pos];
                    sample[1] = current.audio.v[1][pos];
                }
                if (++pos ==
                    (current.duration ? current.duration : 32 * rows(cfg.layer, activeFmt.hz)))
                    active = false;
            }
            if (r.factor < 0)
                fractional_push(&p.fractional, sample[0], sample[1]);
        }
        int statusPort = 0;
        if (cfg.role != Role::Decode) {
            for (int k = 0; k < width; k++)
                u->mOutBuf[k][i] = sounding ? latched[k] : 0;
            u->mOutBuf[width - 8][i] = mark;
            statusPort = width;
            if (cfg.role == Role::Frames)
                return;
            for (int b = 0; b < byteLanes; b++)
                u->mOutBuf[statusPort++][i] =
                    byteRead != byteWrite ? fifo[byteRead++ % sizeof fifo] : -1;
        }
        if (cfg.role == Role::Decode || cfg.role == Role::Pack) {
            if (r.factor > 1) {
                rate_push(&r, sample[0], sample[1]);
                for (int c = 0; c < 2; c++)
                    sample[c] = rate_interpolate(&r, c);
            }
            if (r.factor < 0)
                for (int c = 0; c < 2; c++)
                    sample[c] = fractional_read(&p.fractional, c, -r.clock.phase / r.clock.host);
            for (int c = 0; c < cfg.channels; c++)
                u->mOutBuf[statusPort++][i] = sample[c];
        }
        int state = service->state.load(std::memory_order_relaxed);
        if (cfg.role == Role::Unpack && state == 2 && sounding)
            state = 1;
        u->mOutBuf[statusPort++][i] = state;
        u->mOutBuf[statusPort++][i] = service->frames.load(std::memory_order_relaxed);
        u->mOutBuf[statusPort][i] = localDrops + service->drops.load(std::memory_order_relaxed);
    }
    void legacy(Unit *u, int i) {
        int col = serial.column;
        u->mOutBuf[1][i] = col;
        u->mOutBuf[2][i] = !col && !serial.slot;
        if (cfg.role == Role::Serial) {
            u->mOutBuf[0][i] =
                layer1_tick(&serial, get(u, base, i), get(u, base + 1, i), get(u, base + 2, i),
                            get(u, base + 3, i), get(u, Mode, i) >= .5f, 1);
            return;
        }
        if (!col) {
            written = 0;
            if (!serial.slot)
                for (int b = 0; b < 32; b++) {
                    serialAlloc[b] = get(u, base + 64 + b, i);
                    serialScale[b] = get(u, base + 96 + b, i);
                }
            for (int b = 0; b < std::min(32, cfg.binputs); b++) {
                double a = get(u, base + 32 + b, i);
                int to = !std::isfinite(a) || a < 0 ? b : limit(std::floor(a), 0, 31);
                serialWords[to] = get(u, base + b, i);
                written |= 1u << to;
            }
        }
        u->mOutBuf[0][i] =
            layer1_tick(&serial, serialWords[col], -1, serialAlloc[col], serialScale[col],
                        get(u, Mode, i) >= .5f, (written >> col) & 1);
    }
    void perform(Unit *u, int n) {
        for (int i = 0; i < n; i++) {
            if (cfg.role == Role::Serial || cfg.role == Role::Bands) {
                legacy(u, i);
                continue;
            }
            if (cfg.role == Role::Short) {
                for (int c = 0; c < cfg.channels; c++) {
                    int win = limit(get(u, base + bandCount + c, i), 0, 5),
                        row = limit(get(u, base + bandCount + cfg.channels, i), 0, 17);
                    int first = win == 2 ? 0 : win == 4 ? 2 : win == 5 ? 4 : 32;
                    float db = get(u, base + bandCount + cfg.channels + 1 + 3 * c + row % 3, i);
                    int delta = limit(-db * 4 / 6.020599913, -210, 210);
                    for (int b = 0; b < 32; b++)
                        u->mOutBuf[c * 32 + b][i] = std::clamp(
                            limit(get(u, base + c * 32 + b, i), 0, 210) + (b >= first ? delta : 0),
                            0, 210);
                }
                continue;
            }
            int div = divisor(u, i);
            if (div != lastDiv) {
                int requested = divided(format(cfg.layer, nativeHz, 1), div);
                if (currentHz != requested) {
                    internalPos = 0;
                    phase = 0;
                    firstTick = true;
                }
                lastDiv = div;
                currentHz = requested;
            }
            int hz = currentHz;
            if (cfg.role == Role::Clock) {
                float v = get(u, base, i);
                int m = v == -2 ? internal(hz) : limit(v, -1, 36);
                if (m > 0)
                    clockRow = (m - 1) % (cfg.layer == 3 ? 18 : 36);
                u->mOutBuf[0][i] = clockRow;
                u->mOutBuf[1][i] = m == 1;
                u->mOutBuf[2][i] = m > 0 && (m - 1) % (cfg.layer == 3 ? 18 : 12) == 0;
                u->mOutBuf[3][i] = m;
                continue;
            }
            bool run = get(u, Run, i) > .5f;
            if (run != previousRun) {
                if (cfg.role == Role::Pack) {
                    if (run)
                        ++take;
                    command(run ? Action::Open : Action::Close);
                }
                if (cfg.role == Role::Unpack) {
                    ++gen;
                    active = false;
                    started = false;
                    preroll = 0;
                    byteRead = byteWrite = 0;
                    command(run ? Action::Start : Action::Stop, controls(u, i));
                }
                previousRun = run;
            } else if (retryReader && i == 0) {
                command(run ? Action::Start : Action::Stop, controls(u, i));
            }
            if (cfg.role == Role::Encode)
                inputAudio(u, i, hz);
            else if (cfg.role == Role::Decode && cfg.mode)
                inputBytes(u, i);
            else if (cfg.role != Role::Unpack)
                inputFields(u, i, hz);
            output(u, i, hz);
        }
    }
};

// The server owns the small ticket. Codec construction, file finalization and
// worker joins happen off the audio thread. A synth can be freed during setup.
struct Ticket {
    Config cfg{};
    std::atomic<Front *> front{nullptr};
    Front *retired = nullptr;
    std::atomic<int> refs{2};
};
struct MPEGUnit : Unit {
    Ticket *ticket;
    int marker, byteStart, byteCount, status;
};
Front *const closed = reinterpret_cast<Front *>(uintptr_t(1));
Front *const failed = reinterpret_cast<Front *>(uintptr_t(2));
bool usable(Front *p) {
    return reinterpret_cast<uintptr_t>(p) > 2;
}
void releaseTicket(World *w, void *data) {
    auto t = static_cast<Ticket *>(data);
    if (t->refs.fetch_sub(1) == 1) {
        t->~Ticket();
        RTFree(w, t);
    }
}
bool prepare(World *w, void *data) {
    auto t = static_cast<Ticket *>(data);
    if (t->front.load() == closed)
        return false;
    try {
        std::unique_ptr<Front> p(new Front(t->cfg, !w->mRealTime));
        Front *expected = nullptr;
        if (t->front.compare_exchange_strong(expected, p.get()))
            p.release();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "MPEG Audio: %s\n", e.what());
        Front *expected = nullptr;
        t->front.compare_exchange_strong(expected, failed);
    }
    return false;
}
bool retire(World *, void *data) {
    delete static_cast<Ticket *>(data)->retired;
    return false;
}
void retireFIFO(FifoMsg *m) {
    retire(m->mWorld, m->mData);
}
void releaseFIFO(FifoMsg *m) {
    releaseTicket(m->mWorld, m->mData);
}
void silence(MPEGUnit *u, int n) {
    for (int k = 0; k < u->mNumOutputs; k++)
        std::fill_n(u->mOutBuf[k], n,
                    (k == u->marker || (k >= u->byteStart && k < u->byteStart + u->byteCount))
                        ? -1.f
                        : 0.f);
}
void nextBlock(MPEGUnit *unit, int n) {
    Front *p = unit->ticket ? unit->ticket->front.load(std::memory_order_acquire) : failed;
    if (usable(p)) {
        p->perform(unit, n);
        return;
    }
    silence(unit, n);
    if (p == failed && unit->status >= 0)
        std::fill_n(unit->mOutBuf[unit->status], n, -1.f);
}
void destroy(MPEGUnit *u) {
    auto t = u->ticket;
    if (!t)
        return;
    Front *p = t->front.exchange(closed);
    if (!usable(p)) {
        releaseTicket(u->mWorld, t);
        return;
    }
    t->retired = p;
    if (!u->mWorld->mRealTime) {
        retire(u->mWorld, t);
        releaseTicket(u->mWorld, t);
        return;
    }
    if (ft->fDoAsynchronousCommand(u->mWorld, nullptr, "MPEGAudio", t, retire, nullptr, nullptr,
                                   releaseTicket, 0, nullptr)) {
        FifoMsg msg;
        msg.Set(u->mWorld, retireFIFO, releaseFIFO, t);
        if (!ft->fSendMsgFromRT(u->mWorld, msg))
            Print("MPEG Audio: server cleanup queue exhausted\n");
    }
}
template <Role role, int layer> void construct(MPEGUnit *unit) {
    unit->ticket = nullptr;
    unit->marker = unit->byteStart = unit->status = -1;
    unit->byteCount = 0;
    SETCALC(nextBlock);
    if (unit->mNumInputs < Header) {
        silence(unit, 1);
        return;
    }
    Config c{};
    c.role = role;
    c.layer = layer;
    c.channels = limit(IN0(Channels), 1, 2);
    c.mode = limit(IN0(Mode), 0, 1);
    c.capacity = limit(IN0(Capacity), 1, 1024);
    c.host = unit->mWorld->mSampleRate;
    c.binputs = limit(IN0(ByteInputs), 1, 64);
    int plen = limit(IN0(PathLength), 0, 1024), fw = fieldWidth(layer, c.channels),
        nin = Header + plen, nout;
    if (plen >= 1024 || nin > unit->mNumInputs) {
        silence(unit, 1);
        return;
    }
    for (int i = 0; i < plen; i++) {
        c.path[i] = static_cast<char>(limit(IN0(Header + i), 1, 255));
    }
    const char *restricted = unit->mWorld->mRestrictedPath;
    if (plen && restricted) {
        if (std::strlen(restricted) >= sizeof c.restricted) {
            silence(unit, 1);
            return;
        }
        std::strcpy(c.restricted, restricted);
    }
    switch (role) {
    case Role::Encode:
        nin += c.channels;
        nout = fw + byteLanes + 3;
        break;
    case Role::Decode:
        nin += (c.mode ? c.binputs : fw) + 32 * c.channels;
        nout = c.channels + 3;
        break;
    case Role::Pack:
        nin += fw + 32 * c.channels;
        nout = fw + byteLanes + c.channels + 3;
        break;
    case Role::Unpack:
        nout = fw + byteLanes + 3;
        break;
    case Role::Frames:
        nin += fw + 32 * c.channels;
        nout = fw;
        break;
    case Role::Clock:
        nin++;
        nout = 4;
        break;
    case Role::Short:
        nin += 36 * c.channels + 1;
        nout = 32 * c.channels;
        break;
    case Role::Bands:
        nin += 128;
        nout = 3;
        break;
    case Role::Serial:
        nin += 4;
        nout = 3;
        break;
    }
    if (nin != unit->mNumInputs || nout != unit->mNumOutputs) {
        Print("MPEG Audio: invalid UGen layout (%d/%d inputs, %d/%d outputs)\n", unit->mNumInputs,
              nin, unit->mNumOutputs, nout);
        silence(unit, 1);
        return;
    }
    if (role == Role::Frames || role == Role::Encode || role == Role::Pack || role == Role::Unpack)
        unit->marker = fw - 8;
    if (role == Role::Encode || role == Role::Pack || role == Role::Unpack) {
        unit->byteStart = fw;
        unit->byteCount = byteLanes;
    }
    if (role == Role::Encode || role == Role::Pack || role == Role::Decode || role == Role::Unpack)
        unit->status = nout - 3;
    silence(unit, 1);
    void *mem = RTAlloc(unit->mWorld, sizeof(Ticket));
    if (!mem)
        return;
    auto t = new (mem) Ticket;
    t->cfg = c;
    unit->ticket = t;
    if (!unit->mWorld->mRealTime) {
        prepare(unit->mWorld, t);
        releaseTicket(unit->mWorld, t);
    } else if (ft->fDoAsynchronousCommand(unit->mWorld, nullptr, "MPEGAudio", t, prepare, nullptr,
                                          nullptr, releaseTicket, 0, nullptr)) {
        releaseTicket(unit->mWorld, t);
        t->front.store(failed);
    }
}
template <Role r, int l> void add(const char *name) {
    ft->fDefineUnit(name, sizeof(MPEGUnit), (UnitCtorFunc)construct<r, l>, (UnitDtorFunc)destroy,
                    kUnitDef_CantAliasInputsToOutputs);
}
} // namespace
PluginLoad(MPEGAudio) {
    ft = inTable;
    sb_tables();
    filter_init();
    layer1_init_tables();
    layer3_tables();
    if (!convert_tables()) {
        Print("MPEG Audio: could not prepare subband converter\n");
        return;
    }
#define MPEG_LAYER(N)                                                                              \
    add<Role::Encode, N>("MP" #N "Encode");                                                        \
    add<Role::Decode, N>("MP" #N "Decode");                                                        \
    add<Role::Pack, N>("MP" #N "Pack");                                                            \
    add<Role::Unpack, N>("MP" #N "Unpack");                                                        \
    add<Role::Frames, N>("MP" #N "Frames");                                                        \
    add<Role::Clock, N>("MP" #N "Clock")
    MPEG_LAYER(1);
    MPEG_LAYER(2);
    MPEG_LAYER(3);
    add<Role::Bands, 1>("MP1Bands");
    add<Role::Serial, 1>("MP1Serial");
    add<Role::Short, 3>("MP3Short");
}
