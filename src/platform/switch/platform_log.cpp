// Switch log sink: a file on the SD card, written and flushed line by line by a thread of its own
// (the log is the only channel off the console, so it must survive a hard crash).
//
// Not on the game's thread: a flushed line is an fs round trip of milliseconds, which cost the
// frame rate in fights. The writer shares core 2 with the mixer at the lowest application
// priority. An `ERROR` line waits until it is on the card before its caller goes on.
#include "core/log.h"
#include "core/platform.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <switch.h>
#include <unistd.h>

#if defined(__SWITCH2__)
extern "C" size_t tl_switch2_guard_summary(char* out, size_t size);  // platform_guard.cpp
#endif

namespace tl::core::detail {
namespace {

// What the Switch 2's guard has seen: the services asked for, and anything it refused.
void log_guard() {
#if defined(__SWITCH2__)
    char text[4096];
    if (tl_switch2_guard_summary(text, sizeof text) > 0) sink_write(LogLevel::Info, text);
#endif
}

/// The mixer's core (platform_audio.cpp); the game has 0, and 1 is kept free.
constexpr s32 kWriterCore = 2;

/// The lowest priority an application thread can have; the mixer is 0x3B and
/// the game 0x2C (lower numbers run first).
constexpr s32 kWriterPriority = 0x3F;

struct Writer {
    std::FILE* file = nullptr;
    std::mutex lock;
    std::condition_variable wake;     ///< the writer: lines are waiting
    std::condition_variable written;  ///< an `ERROR` line's caller: it is out
    std::vector<std::string> pending;
    unsigned long long queued = 0;  ///< lines handed over, ever
    unsigned long long done = 0;    ///< of those, the ones on the card
    bool stopping = false;
    /// Lines go to the writer: set once it runs, cleared as sink_close() drains
    /// it. Read under `lock`, unlike `thread`, which only sink_open() and
    /// sink_close() touch, so a line from another thread never races the join.
    bool running = false;
    std::thread::id writer_id;
    std::thread thread;
};

Writer& writer() {
    static Writer w;
    return w;
}

/// The writer thread's own write, and the fallback when there is no thread.
void put(std::FILE* file, const std::vector<std::string>& lines) {
    if (file) {
        for (const std::string& line : lines) {
            std::fputs(line.c_str(), file);
            std::fputc('\n', file);
        }
        std::fflush(file);
#if defined(__SWITCH2__)
        // Committed to the card at most once a second: a game closed from the HOME menu is ended
        // without its files being closed, and what was not committed is lost.
        static auto last_sync = std::chrono::steady_clock::time_point();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_sync >= std::chrono::seconds(1)) {
            (void)fsync(fileno(file));
            last_sync = now;
        }
#endif
    }
    for (const std::string& line : lines) std::printf("%s\n", line.c_str());
}

void run() {
    // Best effort, both: left where libnx started it - core 0, at 0x3B - the
    // writer still runs, whenever the game waits for the swap.
    svcSetThreadPriority(CUR_THREAD_HANDLE, kWriterPriority);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, kWriterCore, 1u << kWriterCore);

    Writer& w = writer();
    std::vector<std::string> batch;
    std::unique_lock<std::mutex> guard(w.lock);
    for (;;) {
        w.wake.wait(guard, [&w] { return !w.pending.empty() || w.stopping; });
        if (w.pending.empty()) break;  // stopping, and nothing left to write
        batch.swap(w.pending);
        const unsigned long long upto = w.queued;
        guard.unlock();
        put(w.file, batch);
        batch.clear();
        guard.lock();
        w.done = upto;
        w.written.notify_all();
    }
}

}  // namespace

void sink_open(const std::string& path_hint) {
    Writer& w = writer();
    if (w.file) return;
    w.file = std::fopen(path_hint.c_str(), "w");

    // Best effort: nxlink stdio for free if it ever works.
    socketInitializeDefault();
    nxlinkStdio();
    setvbuf(stdout, nullptr, _IONBF, 0);

    w.stopping = false;
    try {
        std::thread started(run);
        {
            std::lock_guard<std::mutex> guard(w.lock);
            w.writer_id = started.get_id();
            w.running = true;
        }
        w.thread = std::move(started);
    } catch (const std::exception&) {
        // No thread: sink_write() writes in the caller, as it always used to.
    }
    log_guard();
}

void sink_write(LogLevel level, const std::string& line) {
    Writer& w = writer();
    std::unique_lock<std::mutex> guard(w.lock);
    if (!w.running) {
        put(w.file, {line});
        return;
    }
    // The writer is asleep only on an empty queue, and checks it under the lock before it sleeps:
    // the first line of a batch wakes it, the rest find it awake.
    const bool was_empty = w.pending.empty();
    w.pending.push_back(line);
    const unsigned long long mine = ++w.queued;
    if (was_empty) w.wake.notify_one();
    // Never on the writer's own thread, which would be waiting for itself.
    if (level == LogLevel::Error && std::this_thread::get_id() != w.writer_id) {
        w.written.wait(guard, [&w, mine] { return w.done >= mine; });
    }
}

void sink_close() {
    // Every exit path shuts the log down; the sampler's file is finished first.
    stop_profiler();
    log_guard();
    Writer& w = writer();
    {
        std::lock_guard<std::mutex> guard(w.lock);
        w.stopping = true;
    }
    w.wake.notify_one();
    if (w.thread.joinable()) w.thread.join();
    // The thread has gone; whatever reached the queue after it did goes out
    // here, and an `ERROR` line's caller waiting on it is woken once it is out.
    // All under the lock: from here on a line is written by whoever logs it.
    {
        std::lock_guard<std::mutex> guard(w.lock);
        std::vector<std::string> rest;
        rest.swap(w.pending);
        w.running = false;
        if (!rest.empty()) put(w.file, rest);
        w.done = w.queued;
        w.written.notify_all();
        if (w.file) {
            std::fflush(w.file);
            std::fclose(w.file);
            w.file = nullptr;
        }
    }
    socketExit();
}

}  // namespace tl::core::detail
