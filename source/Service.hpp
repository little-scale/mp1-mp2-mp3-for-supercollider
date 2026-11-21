#pragma once
#include "Codec.hpp"
#include <atomic>
#include <dispatch/dispatch.h>
#include <string>
#include <thread>
namespace mpeg {
enum class Action { Frame, Bytes, Repeat, Open, Close, Start, Stop, Drain };
struct Job {
    Action action = Action::Frame;
    Buffer data;
    Options opt;
    unsigned generation = 0;
    uint64_t take = 0;
    bool record = false;
};
class Service {
  public:
    Service(const Config &, bool offline);
    ~Service();
    bool post(const Job &);
    bool take(Buffer &);
    std::atomic<int> state{0};
    std::atomic<unsigned> drops{0}, frames{0};

  private:
    static constexpr unsigned slots = 16;
    Config cfg;
    bool offline, reading = false, have = false, priorLevel = false;
    Format takeFormat{};
    FILE *file = nullptr;
    std::string temporary;
    Codec codec;
    FileSource source;
    Options lastOptions{};
    Frame previous, converted, mixed;
    std::unique_ptr<Frame[]> anchors;
    unsigned lastMasks[2]{}, generation = 0;
    std::unique_ptr<Job[]> jobs;
    std::unique_ptr<Buffer[]> outputs;
    Buffer scratch, repack;
    std::atomic<unsigned> ir{0}, iw{0}, orr{0}, ow{0};
    std::atomic<bool> closing{false};
    std::atomic<uint64_t> failedThrough{0};
    uint64_t activeTake = 0;
    dispatch_semaphore_t wake = nullptr;
    std::thread worker;
    void loop();
    void process(const Job &);
    void guarded(const Job &);
    bool readNext();
    void publish(const Buffer &);
    void fail(const char *);
    void closeFile(bool commit);
    void edited(const Job &);
    void deliver(const Frame &, const Options &, bool record, bool preserve);
    void drain(bool record);
};
} // namespace mpeg
