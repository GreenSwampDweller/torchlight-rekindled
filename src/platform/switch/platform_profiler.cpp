// Where the game's thread spends its time on the console, sampled: a thread on another core pauses
// it, reads its registers (`svcGetThreadContext3`), walks the frame-pointer chain, and resumes it.
//
// Only when asked at start-up: a line `profile=1` in the game folder's config.txt, or a file
// `profile` (or `profile.txt`) in the game's folder (platform_data_root) or in
// `sdmc:/switch/torchlight/`. Samples go to `profile.bin` in that folder as image offsets;
// profiler_mark() labels what follows.
//
// Nothing that can take a lock (malloc, stdio, logging) runs while the game's thread is paused.
#include "core/platform.h"

#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include <dirent.h>
#include <strings.h>
#include <switch.h>

extern "C" void _start(void);

namespace tl::core {
namespace {

#ifndef TL_RUN_TAG
#define TL_RUN_TAG ""
#endif
/// profile.bin, or profile-<tag>.bin for a tagged build (cmake/switch.cmake).
const char* const kOutName = sizeof(TL_RUN_TAG) > 1 ? "profile-" TL_RUN_TAG ".bin" : "profile.bin";

bool file_exists(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

/// The folder whose `profile` (or `profile.txt`, as Windows tends to save it) asks for sampling:
/// the game's own first, then the fixed one older notes name. Empty when neither asks.
/// Whether `config.txt` in `folder` has a line `profile=1` (spaces and case aside).
bool config_asks(const std::string& folder) {
    std::FILE* f = std::fopen((folder + "config.txt").c_str(), "rb");
    if (f == nullptr) return false;
    bool asked = false;
    char line[256];
    while (!asked && std::fgets(line, sizeof line, f) != nullptr) {
        std::string text;
        for (const char* c = line; *c != '\0' && *c != '#'; ++c) {
            if (std::isspace(static_cast<unsigned char>(*c)) == 0)
                text += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        }
        asked = text == "profile=1";
    }
    std::fclose(f);
    return asked;
}

std::string profile_folder() {
    if (config_asks(platform_data_root())) return platform_data_root();
    for (const std::string& folder :
         {platform_data_root(), std::string("sdmc:/switch/torchlight/")}) {
        if (file_exists(folder + "profile") || file_exists(folder + "profile.txt")) return folder;
    }
    return std::string();
}

/// The names in `folder` that start with "profile", for the log when nothing asked: a flag
/// file saved under another name (`profile.txt.txt`) shows there.
std::string profile_lookalikes(const std::string& folder) {
    std::string names;
    DIR* dir = opendir(folder.c_str());
    if (dir == nullptr) return names;
    while (const dirent* entry = readdir(dir)) {
        if (strncasecmp(entry->d_name, "profile", 7) != 0) continue;
        if (!names.empty()) names += ", ";
        names += entry->d_name;
    }
    closedir(dir);
    return names;
}
/// 500 samples a second: enough for a function at 1 % to collect a few
/// hundred samples in a minute, and a pause the game does not feel.
constexpr s64 kIntervalNs = 2000000;
constexpr int kMaxDepth = 40;
constexpr int kBufferedSamples = 1024;
constexpr u16 kMarker = 0xffff;

/// One sample: how many offsets follow, the thread it is of (0 the game's,
/// n the n-th thread the process created, see __wrap_threadCreate), then the
/// program counter, the link register and the return addresses up the frame
/// chain.
struct Sample {
    u16 depth = 0;
    u16 thread = 0;
    u32 offsets[kMaxDepth] = {};
};

/// A thread sampled, and its stack's bounds once known.
struct Target {
    Handle handle = INVALID_HANDLE;
    u64 stack_lo = 0;
    u64 stack_hi = 0;
};

/// Every other thread the process created, recorded as it is made: Mesa's
/// glthread worker among them (libnx's pthread_create goes through
/// threadCreate), so the driver's own thread can be sampled too.
constexpr int kMaxThreads = 32;
Handle g_threads[kMaxThreads] = {};
std::atomic<int> g_thread_count{0};

struct State {
    Thread thread{};
    Target main;
    Target others[kMaxThreads];
    bool all_threads = false;  ///< profile_threads exists beside the profile flag
    std::atomic<bool> running{false};
    std::FILE* out = nullptr;
    uintptr_t base = 0;
    Sample buffer[kBufferedSamples];
    int buffered = 0;
    u64 samples = 0;
    u64 failures = 0;
    // profiler_mark()'s label, handed over by a sequence number.
    char label[64] = {};
    std::atomic<u32> label_seq{0};
    u32 label_written = 0;
};

State* g_state = nullptr;

u32 offset_of(u64 address, uintptr_t base) { return static_cast<u32>(address - base); }

void flush(State& s) {
    if (s.out != nullptr && s.buffered > 0) {
        for (int i = 0; i < s.buffered; ++i) {
            const Sample& sample = s.buffer[i];
            std::fwrite(&sample.depth, sizeof(u16), 1, s.out);
            std::fwrite(&sample.thread, sizeof(u16), 1, s.out);
            std::fwrite(sample.offsets, sizeof(u32), sample.depth, s.out);
        }
    }
    s.buffered = 0;
}

void write_marker(State& s) {
    const u32 seq = s.label_seq.load(std::memory_order_acquire);
    if (seq == s.label_written || s.out == nullptr) return;
    flush(s);
    char label[64];
    std::memcpy(label, s.label, sizeof label);
    label[sizeof label - 1] = '\0';
    s.label_written = seq;
    const u16 marker = kMarker;
    const u16 length = static_cast<u16>(std::strlen(label));
    std::fwrite(&marker, sizeof marker, 1, s.out);
    std::fwrite(&length, sizeof length, 1, s.out);
    std::fwrite(label, 1, length, s.out);
}

bool in_stack(const Target& t, u64 address) {
    return address >= t.stack_lo && address + 16 <= t.stack_hi && (address & 15) == 0;
}

/// Pause, read, resume. Only system calls and stack reads between the first
/// and the last line.
bool take_sample(const State& s, Target& t, Sample& sample) {
    if (R_FAILED(svcSetThreadActivity(t.handle, ThreadActivity_Paused))) return false;
    ThreadContext context;
    const bool read = R_SUCCEEDED(svcGetThreadContext3(&context, t.handle));
    int depth = 0;
    if (read) {
        if (t.stack_hi == 0) {
            MemoryInfo info{};
            u32 page = 0;
            if (R_SUCCEEDED(svcQueryMemory(&info, &page, context.sp))) {
                t.stack_lo = info.addr;
                t.stack_hi = info.addr + info.size;
            }
        }
        sample.offsets[depth++] = offset_of(context.pc.x, s.base);
        sample.offsets[depth++] = offset_of(context.lr, s.base);
        u64 fp = context.fp;
        while (depth < kMaxDepth && in_stack(t, fp)) {
            const u64* frame = reinterpret_cast<const u64*>(fp);
            const u64 next = frame[0];
            const u64 ret = frame[1];
            if (ret == 0) break;
            sample.offsets[depth++] = offset_of(ret, s.base);
            if (next <= fp) break;  // the chain only climbs
            fp = next;
        }
    }
    svcSetThreadActivity(t.handle, ThreadActivity_Runnable);
    sample.depth = static_cast<u16>(depth);
    return read && depth > 0;
}

void profiler_main(void* arg) {
    State& s = *static_cast<State*>(arg);
    while (s.running.load(std::memory_order_acquire)) {
        svcSleepThread(kIntervalNs);
        write_marker(s);
        // The game's thread; the others only when the card asks (profile_threads): pausing Mesa's
        // worker while it presents has taken the console down.
        const int others =
            s.all_threads ? std::min(g_thread_count.load(std::memory_order_acquire), kMaxThreads)
                          : 0;
        for (int i = -1; i < others; ++i) {
            Target& t = i < 0 ? s.main : s.others[i];
            if (i >= 0) {
                if (g_threads[i] == s.thread.handle) continue;
                if (t.handle != g_threads[i]) t = Target{g_threads[i]};
            }
            Sample& sample = s.buffer[s.buffered];
            sample.thread = static_cast<u16>(i + 1);
            if (take_sample(s, t, sample)) {
                if (i < 0) ++s.samples;
                if (++s.buffered == kBufferedSamples) flush(s);
            } else if (i < 0) {
                ++s.failures;
            }
        }
    }
    flush(s);
}

}  // namespace

// Every libnx thread goes through here (--wrap=threadCreate, cmake/switch.cmake),
// pthread ones included, so the sampler knows the driver's threads too.
extern "C" Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                                      size_t stack_sz, int prio, int cpuid);
extern "C" Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                                      size_t stack_sz, int prio, int cpuid) {
#if defined(__SWITCH2__)
    // A thread asking for the default core (libounce's pthread_create: Mesa's threaded-context
    // and flush workers, and ours before they place themselves) starts on core 1, not on the game
    // thread's core 0, so the driver works beside the game instead of taking turns with it. The
    // Switch 1's patched Mesa pins its glthread worker to core 1 itself.
    constexpr int kDefaultCore = -2;
    constexpr int kDriverCore = 1;
    Result rc = cpuid == kDefaultCore
                    ? __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, kDriverCore)
                    : __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
    if (R_FAILED(rc) && cpuid == kDefaultCore)
        rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
#else
    const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
#endif
    if (R_SUCCEEDED(rc)) {
        const int i = g_thread_count.load(std::memory_order_relaxed);
        if (i < kMaxThreads) {
            g_threads[i] = t->handle;
            g_thread_count.store(i + 1, std::memory_order_release);
        }
    }
    return rc;
}

void start_profiler() {
    if (g_state != nullptr) return;
    const std::string folder = profile_folder();
    if (folder.empty()) {
        const std::string seen = profile_lookalikes(platform_data_root());
        TL_LOG_INFO(
            "profiler: off (no profile=1 in %sconfig.txt, no 'profile' file there or in "
            "sdmc:/switch/torchlight/; names there starting with 'profile': %s)",
            platform_data_root().c_str(), seen.empty() ? "none" : seen.c_str());
        return;
    }
    const std::string out_path = folder + kOutName;

    auto* s = new State();
    s->base = reinterpret_cast<uintptr_t>(&_start);
    // The game's thread is the one calling.
    s->main.handle = threadGetCurHandle();
    s->all_threads = file_exists(folder + "profile_threads");
    s->out = std::fopen(out_path.c_str(), "wb");
    if (s->out == nullptr) {
        TL_LOG_WARN("profiler: cannot write %s; not sampling", out_path.c_str());
        delete s;
        return;
    }
    std::fwrite("TLPROF1\n", 1, 8, s->out);
    s->running.store(true, std::memory_order_release);
    // Core 2, above the mixer and the log writer that share it, so a sample
    // is taken when it is due.
    if (R_FAILED(threadCreate(&s->thread, profiler_main, s, nullptr, 0x10000, 0x20, 2)) ||
        R_FAILED(threadStart(&s->thread))) {
        TL_LOG_WARN("profiler: no thread; not sampling");
        std::fclose(s->out);
        delete s;
        return;
    }
    g_state = s;
    TL_LOG_INFO("profiler: sampling the game's thread every %lld us into %s",
                static_cast<long long>(kIntervalNs / 1000), out_path.c_str());
}

void stop_profiler() {
    State* s = g_state;
    if (s == nullptr) return;
    g_state = nullptr;
    s->running.store(false, std::memory_order_release);
    threadWaitForExit(&s->thread);
    threadClose(&s->thread);
    std::fclose(s->out);
    TL_LOG_INFO("profiler: %llu samples written, %llu missed",
                static_cast<unsigned long long>(s->samples),
                static_cast<unsigned long long>(s->failures));
    delete s;
}

void profiler_mark(const char* label) {
    State* s = g_state;
    if (s == nullptr || label == nullptr) return;
    // A label torn by a write during the copy only costs one wrong marker.
    std::strncpy(s->label, label, sizeof s->label - 1);
    s->label_seq.fetch_add(1, std::memory_order_release);
}

}  // namespace tl::core
