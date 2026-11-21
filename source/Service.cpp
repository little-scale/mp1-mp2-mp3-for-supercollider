#include "Service.hpp"
#include <stdexcept>
#include <unistd.h>
#include <chrono>
namespace mpeg {
Service::Service(const Config &c, bool nrt)
    : cfg(c), offline(nrt), codec(c.layer), jobs(new Job[slots]), outputs(new Buffer[slots]) {
    if (!offline) {
        wake = dispatch_semaphore_create(0);
        if (!wake)
            throw std::bad_alloc();
        try {
            worker = std::thread(&Service::loop, this);
        } catch (...) {
            dispatch_release(wake);
            throw;
        }
    }
}
Service::~Service() {
    if (!offline) {
        closing = true;
        dispatch_semaphore_signal(wake);
        worker.join();
        dispatch_release(wake);
    } else if (file) {
        try {
            drain(true);
            closeFile(true);
        } catch (...) {
            closeFile(false);
        }
    }
}
void Service::fail(const char *reason) {
    std::fprintf(stderr, "MPEG Audio: %s%s%s\n", reason ? reason : "stream error",
                 cfg.path[0] ? " | " : "", cfg.path);
    state = -1;
    reading = false;
    closeFile(false);
}
void Service::closeFile(bool commit) {
    if (!file)
        return;
    if (commit && activeTake && activeTake <= failedThrough.load()) {
        state = -1;
        commit = false;
    }
    bool good = std::fclose(file) == 0;
    file = nullptr;
    if (commit && good && takeFormat.hz) {
        good = std::rename(temporary.c_str(), cfg.path) == 0;
    } else if (commit)
        good = false;
    if (!good || !commit)
        std::remove(temporary.c_str());
    if (commit) {
        state = good ? 2 : -1;
        if (!good)
            std::fprintf(stderr, "MPEG Audio: take could not be finalized: %s\n", cfg.path);
    }
    temporary.clear();
    takeFormat = {};
}
bool Service::post(const Job &j) {
    if (offline) {
        guarded(j);
        return true;
    }
    unsigned w = iw.load(std::memory_order_relaxed);
    bool data =
        j.action == Action::Frame || j.action == Action::Bytes || j.action == Action::Repeat;
    if (w - ir.load(std::memory_order_acquire) >= (data ? slots - 2 : slots)) {
        drops++;
        if (cfg.role == Role::Pack && cfg.path[0] &&
            (j.record || j.action == Action::Open || j.action == Action::Close ||
             j.action == Action::Drain)) {
            failedThrough.store(j.take);
            dispatch_semaphore_signal(wake);
        }
        return false;
    }
    jobs[w % slots] = j;
    iw.store(w + 1, std::memory_order_release);
    dispatch_semaphore_signal(wake);
    return true;
}
void Service::publish(const Buffer &b) {
    unsigned w = ow.load(std::memory_order_relaxed);
    if (w - orr.load(std::memory_order_acquire) == slots) {
        drops++;
        return;
    }
    outputs[w % slots] = b;
    outputs[w % slots].generation = generation;
    ow.store(w + 1, std::memory_order_release);
}
bool Service::take(Buffer &b) {
    unsigned r = orr.load(std::memory_order_relaxed);
    if (offline && reading && r == ow.load()) {
        for (;;) {
            try {
                if (readNext() || !reading)
                    break;
            } catch (const std::exception &e) {
                fail(e.what());
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    if (r == ow.load(std::memory_order_acquire))
        return false;
    b = outputs[r % slots];
    orr.store(r + 1, std::memory_order_release);
    if (!offline && cfg.role == Role::Unpack)
        dispatch_semaphore_signal(wake);
    return true;
}
void Service::loop() {
    for (;;) {
        unsigned r = ir.load(std::memory_order_relaxed);
        while (r != iw.load(std::memory_order_acquire)) {
            guarded(jobs[r % slots]);
            ir.store(++r, std::memory_order_release);
        }
        if (file && activeTake <= failedThrough.load())
            fail("recording queue overflow");
        if (closing.load() && r == iw.load(std::memory_order_acquire))
            break;
        if (reading && ow.load() - orr.load() < slots) {
            try {
                if (readNext())
                    continue;
            } catch (const std::exception &e) {
                fail(e.what());
            }
            if (reading) {
                std::this_thread::sleep_for(std::chrono::microseconds(250));
                continue;
            }
        }
        dispatch_semaphore_wait(wake, DISPATCH_TIME_FOREVER);
    }
    if (file) {
        try {
            drain(true);
            closeFile(true);
        } catch (...) {
            closeFile(false);
        }
    }
}
bool Service::readNext() {
    scratch.hasAudio = false;
    scratch.bytes.size = 0;
    int rc = source.next(scratch);
    if (rc == 1) {
        if (!scratch.bytes.size) {
            if (!codec.pack(scratch.frame, 0, repack, true))
                throw std::runtime_error("could not serialize file fields");
            scratch.bytes = repack.bytes;
            scratch.frame.fmt.kbps = repack.frame.fmt.kbps;
        }
        publish(scratch);
        frames++;
        return true;
    }
    if (rc == 2) {
        reading = false;
        state = 2;
    }
    if (rc < 0)
        fail(source.error);
    return false;
}
void Service::guarded(const Job &j) {
    try {
        process(j);
    } catch (const std::exception &e) {
        fail(e.what());
    } catch (...) {
        fail("codec operation failed");
    }
}
void Service::process(const Job &j) {
    switch (j.action) {
    case Action::Open: {
        if (file) {
            drain(true);
            closeFile(true);
        }
        activeTake = j.take;
        if (activeTake <= failedThrough.load()) {
            state = -1;
            return;
        }
        codec.resetConversion();
        takeFormat = {};
        frames = 0;
        if (!cfg.path[0]) {
            state = 1;
            return;
        }
        temporary = std::string(cfg.path) + ".XXXXXX";
        int fd = mkstemp(temporary.data());
        if (fd < 0)
            throw std::runtime_error("cannot create recording");
        file = fdopen(fd, "wb");
        if (!file) {
            close(fd);
            std::remove(temporary.c_str());
            throw std::runtime_error("cannot open recording");
        }
        state = 1;
        return;
    }
    case Action::Close:
        drain(true);
        closeFile(true);
        if (state >= 0)
            state = 2;
        return;
    case Action::Start:
        generation = j.generation;
        frames = 0;
        codec.resetStream();
        reading = source.open(cfg.path, cfg.layer, j.opt.divisor);
        if (!reading)
            fail(source.error);
        else
            state = 1;
        return;
    case Action::Stop:
        generation = j.generation;
        reading = false;
        source.close();
        state = 0;
        return;
    case Action::Drain:
        drain(file != nullptr);
        return;
    default:
        break;
    }
    generation = j.generation;
    if (cfg.role == Role::Encode) {
        scratch.hasAudio = false;
        if (codec.encode(j.data.audio, j.data.frame.fmt, scratch)) {
            publish(scratch);
            state = 1;
            frames++;
        }
        return;
    }
    if (cfg.role == Role::Decode && cfg.layer == 3 && cfg.mode) {
        if (codec.decodeMp3Bytes(j.action == Action::Bytes ? &j.data.bytes : nullptr, j.opt, cfg,
                                 scratch)) {
            state = 1;
            frames++;
            publish(scratch);
        }
        return;
    }
    edited(j);
}
void Service::edited(const Job &j) {
    const Frame *live = &j.data.frame;
    if (j.action == Action::Bytes) {
        if (!codec.unpack(j.data.bytes, mixed))
            return;
        if (mixed.fmt.channels == 1 && cfg.channels == 2) {
            if (cfg.layer == 3)
                for (int g = 0; g < rows(3, mixed.fmt.hz) / 18; g++)
                    mixed.mp[g][1] = mixed.mp[g][0];
            else {
                std::memcpy(mixed.sub.allocation[1], mixed.sub.allocation[0], 32);
                for (int g = 0; g < 3; g++)
                    std::memcpy(mixed.sub.scale[g][1], mixed.sub.scale[g][0], 32);
                for (int r = 0; r < rows(cfg.layer, mixed.fmt.hz); r++)
                    std::memcpy(mixed.sub.q[r][1], mixed.sub.q[r][0], 64);
            }
            mixed.fmt.channels = 2;
            mixed.fmt.mode = 0;
            if (cfg.layer != 3)
                mixed.sub.fmt = mixed.fmt;
        }
        live = &mixed;
    }
    if (j.action == Action::Repeat) {
        if (!have)
            return;
        mixed = previous;
        if (!j.opt.freeze)
            for (int c = 0; c < mixed.fmt.channels; c++)
                for (int b = 0; b < 32; b++)
                    if (!(j.opt.masks[c] & (1u << b))) {
                        if (cfg.layer != 3)
                            mixed.sub.allocation[c][b] = 0;
                        else
                            for (int g = 0; g < rows(3, mixed.fmt.hz) / 18; g++)
                                std::fill_n(mixed.mp[g][c].q + 18 * b, 18, 0.f);
                    }
        live = &mixed;
    } else {
        if (have &&
            (previous.fmt.hz != live->fmt.hz || previous.fmt.channels != live->fmt.channels)) {
            have = false;
            lastMasks[0] = lastMasks[1] = 0;
        }
        if (have && j.opt.freeze)
            live = &previous;
        else if (have && (j.opt.masks[0] || j.opt.masks[1])) {
            if (!anchors)
                anchors = std::make_unique<Frame[]>(64);
            const Frame *choice[2][32]{};
            for (int c = 0; c < live->fmt.channels; c++)
                for (int b = 0; b < 32; b++)
                    if (j.opt.masks[c] & (1u << b)) {
                        Frame &a = anchors[c * 32 + b];
                        if (!(lastMasks[c] & (1u << b)) || priorLevel != j.opt.level)
                            a = previous;
                        choice[c][b] = &a;
                    }
            mixFrames(converted, *live, choice, j.opt.level);
            live = &converted;
        }
        if (live != &previous)
            previous = *live;
        if (!j.opt.freeze) {
            lastMasks[0] = j.opt.masks[0];
            lastMasks[1] = j.opt.masks[1];
            priorLevel = j.opt.level;
        }
        have = true;
        live = &previous;
    }
    if (file && j.record && takeFormat.hz &&
        (takeFormat.hz != divided(live->fmt, j.opt.divisor) ||
         takeFormat.channels != live->fmt.channels)) {
        drain(true);
        closeFile(true);
    }
    int target = cfg.role == Role::Decode && cfg.mode
                     ? byteTarget(cfg.layer, live->fmt.hz,
                                  nativeRate(cfg.layer, cfg.host, j.opt.divisor), j.opt.divisor)
                     : divided(live->fmt, j.opt.divisor);
    lastOptions = j.opt;
    if (codec.convert(*live, target, converted))
        deliver(converted, j.opt, j.record, live->fmt.pad >= 0 && live->fmt.hz == target);
}
void Service::deliver(const Frame &f, const Options &opt, bool record, bool preserve) {
    scratch.bytes.size = 0;
    scratch.hasAudio = false;
    if (cfg.role == Role::Pack || opt.kbps > 0) {
        if (!codec.pack(f, opt.kbps, scratch, preserve))
            throw std::runtime_error("cannot fit MPEG frame");
    } else
        scratch.frame = f;
    if (file && record) {
        if (std::fwrite(scratch.bytes.data, 1, scratch.bytes.size, file) !=
            static_cast<size_t>(scratch.bytes.size))
            throw std::runtime_error("disk write failed");
        takeFormat = scratch.frame.fmt;
        frames++;
    }
    if (cfg.role == Role::Decode || opt.monitor) {
        codec.synth(scratch.frame, scratch.audio);
        scratch.hasAudio = true;
    }
    if (cfg.role == Role::Decode) {
        state = 1;
        frames++;
    }
    if (cfg.role == Role::Pack) {
        // Expose the edited bank for downstream patches. The monitor and bytes
        // reflect the allocation and quantization that actually fit the frame.
        int fittedBitrate = scratch.frame.fmt.kbps;
        scratch.frame = f;
        scratch.frame.fmt.kbps = fittedBitrate;
        if (cfg.layer != 3)
            scratch.frame.sub.fmt = scratch.frame.fmt;
        if (!cfg.path[0] && record)
            frames++;
    }
    publish(scratch);
}
void Service::drain(bool record) {
    while (codec.finish(converted))
        deliver(converted, lastOptions, record, false);
    codec.resetConversion();
}
} // namespace mpeg
