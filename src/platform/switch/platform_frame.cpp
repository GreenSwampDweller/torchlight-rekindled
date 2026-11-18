// Switch frame lifecycle: no window system, only the applet lifecycle. appletMainLoop() returns
// false when the user backs out to the home menu or the system tears the applet down; a program
// that ignores it is killed (and loses the tail of the log).
#include "core/platform.h"

#include "core/log.h"

#include <malloc.h>
#include <switch.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace tl::core {

// libnx's hooks around main(): called once the file system service is up and
// once it is about to go. The .nro's RomFS holds OGRE's shader library
// (cmake/switch.cmake), read as `romfs:/RTShaderLib`.
// A failed mount is reported by the renderer, which then finds no RTShaderLib.
extern "C" void userAppInit(void) {
    (void)romfsInit();
#if defined(__SWITCH2__)
    // The GL driver's shader cache in the game's folder (`ouncevk/`), not its default
    // `sdmc:/config/ouncevk`, where the guard refuses writes. The arguments are read by now.
    setenv("MESA_SHADER_CACHE_DIR", (platform_data_root() + "ouncevk").c_str(), 0);
#endif
}

// The loader stays in this process after the game (hbmenu comes back in it): a load cut short
// must not leave the boost on for what runs next.
extern "C" void userAppExit(void) {
#if defined(__SWITCH2__)
    (void)appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
#endif
    romfsExit();
}

// libnx's copy of the arguments the loader started the .nro with.
extern "C" {
extern int __system_argc;
extern char** __system_argv;
#if defined(__SWITCH2__)
bool tl_switch2_write_allowed(const char* path);  // platform_guard.cpp
#endif
}

// The homebrew's own directory on the SD card: the folder the .nro was started from
// (hbmenu passes its path as argv[0]), so copies in two folders keep two games, saves
// and logs apart. `sdmc:/switch/torchlight/` when the loader does not say.
const std::string& platform_data_root() {
    static const std::string root = [] {
        std::string path =
            __system_argc > 0 && __system_argv != nullptr && __system_argv[0] != nullptr
                ? __system_argv[0]
                : "";
        if (path.rfind('/', 0) == 0) path = "sdmc:" + path;
        const size_t slash = path.find_last_of('/');
        if (path.rfind("sdmc:/", 0) != 0 || slash == std::string::npos) {
            return std::string("sdmc:/switch/torchlight/");
        }
#if defined(__SWITCH2__)
        // The Switch 2 writes only inside /switch/torchlight*/ (platform_guard.cpp): a copy
        // started from anywhere else keeps its saves and logs in the default folder.
        if (!tl_switch2_write_allowed(path.substr(0, slash + 1).c_str())) {
            return std::string("sdmc:/switch/torchlight/");
        }
#endif
        return path.substr(0, slash + 1);
    }();
    return root;
}

#if defined(__SWITCH2__)
namespace {

/// config.txt's `docked_resolution` (1080, 1440 or 2160): the lines of the picture drawn docked.
/// 1080 when the file does not say.
unsigned docked_lines() {
    std::FILE* f = std::fopen((platform_data_root() + "config.txt").c_str(), "rb");
    if (f == nullptr) return 1080;
    unsigned lines = 1080;
    char line[256];
    const char key[] = "docked_resolution=";
    while (std::fgets(line, sizeof line, f) != nullptr) {
        std::string text;
        for (const char* c = line; *c != '\0' && *c != '#'; ++c) {
            if (std::isspace(static_cast<unsigned char>(*c)) == 0)
                text += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        }
        if (text.rfind(key, 0) == 0)
            lines = static_cast<unsigned>(std::atoi(text.c_str() + sizeof key - 1));
    }
    std::fclose(f);
    return lines;
}

unsigned g_docked_lines = 1080;
unsigned g_fps_limit = 0;  ///< fps_limit: 0 for none
int g_mode = -1;           ///< the console's mode the window was last sized for
bool g_reapply = false;    ///< a setting changed: size and frame rate again
u32 g_window_width = 0;
u32 g_window_height = 0;

bool docked() { return appletGetOperationMode() == AppletOperationMode_Console; }

/// The picture's size for the console's mode now: 1920x1080 in the hand; docked, what
/// `docked_resolution` asks (1080, 1440 or 2160), no larger than the TV's output.
void wanted_size(u32& width, u32& height) {
    width = 1920;
    height = 1080;
    if (!docked()) return;
    s32 tv_width = 0;
    s32 tv_height = 0;
    if (R_FAILED(appletGetDefaultDisplayResolution(&tv_width, &tv_height)) || tv_height <= 0)
        return;
    if (g_docked_lines >= 2160) {
        width = static_cast<u32>(tv_width);
        height = static_cast<u32>(tv_height);
    } else if (g_docked_lines >= 1440 && tv_height >= 1440) {
        width = 2560;
        height = 1440;
    }
}

/// The window to `width` x `height`, a fixed size: the system shows a picture larger than the
/// screen as a crop, not scaled, so the game draws at the screen's own size.
void size_window(u32 width, u32 height) {
    (void)nwindowSetDimensions(nwindowGetDefault(), width, height);
    g_window_width = width;
    g_window_height = height;
    g_mode = static_cast<int>(appletGetOperationMode());
}

// DIVERGENCE (platform): the Switch 2's screen runs at up to 120 Hz, and its window stays at 60
// unless asked. Up to 120 frames a second at 120 Hz and 60 at 60 Hz, or fps_limit where lower;
// variable refresh rate on, so rates between them pace evenly where the screen has it. A picture
// taller than 1440 lines asks for no 120 Hz: a TV takes 120 Hz only at 1080p or 1440p, and a 4K
// picture under that request was shown as a zoomed-in crop.
void request_frame_rates() {
    u32 at_120hz = g_fps_limit == 0 || g_fps_limit > 120 ? 120 : g_fps_limit;
    const u32 at_60hz = g_fps_limit == 0 || g_fps_limit > 60 ? 60 : g_fps_limit;
    if (g_window_height > 1440 && at_120hz > 60) at_120hz = 60;
    const Result rc = nwindowSetFrameRates(nwindowGetDefault(), at_120hz, at_60hz, true);
    TL_LOG_INFO(
        "display: %s, window %ux%u, docked_resolution=%u; up to %u fps at 120 Hz, %u at "
        "60 Hz (0x%x)",
        docked() ? "docked" : "handheld", g_window_width, g_window_height, g_docked_lines, at_120hz,
        at_60hz, static_cast<unsigned>(rc));
}

}  // namespace

// The screen's size as the game starts (wanted_size), the window set to it. The game owns the
// window's size from here on: OGRE leaves it alone, and tl_switch2_follow_mode changes it.
extern "C" void tl_switch2_screen_size(unsigned* width, unsigned* height) {
    g_docked_lines = docked_lines();
    u32 w = 0;
    u32 h = 0;
    wanted_size(w, h);
    size_window(w, h);
    *width = w;
    *height = h;
}

// Once a frame: when the console docks or undocks, or a setting changed (tl_switch2_set_display),
// the window to the new mode's size and the frame rate asked again. True, with the new size, when
// the window's size changed: the renderer then follows the surface to it.
extern "C" bool tl_switch2_follow_mode(unsigned* width, unsigned* height) {
    if (static_cast<int>(appletGetOperationMode()) == g_mode && !g_reapply) return false;
    g_reapply = false;
    u32 w = 0;
    u32 h = 0;
    wanted_size(w, h);
    const bool resized = w != g_window_width || h != g_window_height;
    size_window(w, h);
    request_frame_rates();
    if (!resized) return false;
    *width = w;
    *height = h;
    return true;
}

// config.txt's docked_resolution, or as the mods page last set it.
extern "C" unsigned tl_switch2_docked_lines() {
    return g_docked_lines >= 2160 ? 2160 : g_docked_lines >= 1440 ? 1440 : 1080;
}

// The docked resolution (1080, 1440, 2160) and the frame-rate limit (0 none), changed while the
// game runs; applied at the next frame, as a dock change is.
extern "C" void tl_switch2_set_display(unsigned docked_lines, unsigned fps_limit) {
    g_docked_lines = docked_lines;
    g_fps_limit = fps_limit;
    g_reapply = true;
}
#endif

// OGRE makes the window itself.
void platform_window_params(std::map<std::string, std::string>& params) { (void)params; }

// The system's own error dialog (libnx's error applet): `summary` on the
// dialog, `detail` behind its Details button. It returns when it is closed.
void platform_error_message(const std::string& summary, const std::string& detail) {
    TL_LOG_ERROR("%s", summary.c_str());
#if defined(__SWITCH2__)
    // DIVERGENCE (platform): no system applet under kaiHEN, whose error dialog may file the
    // error with the system; the message on the text console until + or B.
    consoleInit(nullptr);
    std::printf("%s\n\n%s\n\nPress + or B to close.\n", summary.c_str(), detail.c_str());
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_B)) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
#else
    ErrorApplicationConfig config;
    if (R_SUCCEEDED(errorApplicationCreate(&config, summary.c_str(), detail.c_str()))) {
        (void)errorApplicationShow(&config);
    }
#endif
}

void pump_events() {
    // Nothing to dispatch: the applet's messages are should_keep_running's.
    note_events_pumped();
}

// `appletMainLoop` says false once, as it reads the exit request; kept, so a request read
// during loading (keep_responsive) still ends the frame loop.
bool should_keep_running() {
    static bool running = true;
    if (running) running = appletMainLoop();
    return running;
}

#if defined(__SWITCH2__)
// fps_limit, for the window's frame rate (request_frame_rates).
void platform_set_frame_rate(float fps) {
    g_fps_limit = fps >= 1.0f ? static_cast<unsigned>(fps) : 0;
    request_frame_rates();
}
#else
// The Switch's display is its own: 60 Hz, which the swap already waits for.
void platform_set_frame_rate(float fps) { (void)fps; }
#endif

void platform_sleep_until(std::chrono::steady_clock::time_point deadline) {
    std::this_thread::sleep_until(deadline);
}

bool memory_usage(unsigned long long& used, unsigned long long& total) {
    // `mallinfo().uordblks`, what the program has allocated (size_t fields, no overflow), with
    // `arena` as the upper bound. Not `svcGetInfo(InfoType_UsedMemorySize)`: hbloader reserves the
    // whole heap up front, so that is the reservation. GPU buffers mapped by Mesa are not counted.
    const struct mallinfo info = mallinfo();
    u64 ceiling = 0;
    if (R_FAILED(svcGetInfo(&ceiling, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
        return false;
    }
    used = static_cast<unsigned long long>(info.uordblks);
    total = static_cast<unsigned long long>(ceiling);
    return true;
}

void set_loading_boost(bool on) {
    // What the system was last told, so a frame that finds boost already off
    // costs a comparison rather than an IPC round trip.
    static bool active = false;
    // Once refused, always refused: the reasons (firmware older than 7.0.0, or
    // an applet rather than an application) do not change during a run.
    static bool refused = false;
    if (on == active || refused) return;

    // libnx refuses firmware older than 7.0.0 itself, without calling the
    // service; the service refuses a process that is not an application.
    const Result rc = appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
    if (R_FAILED(rc)) {
        if (on) {
            refused = true;
            TL_LOG_INFO("cpu boost unavailable (0x%x); loading at the normal clock",
                        static_cast<unsigned>(rc));
        } else {
            // Left on, it would hold the GPU down through gameplay, so `active`
            // stays true and the next call tries again.
            TL_LOG_WARN("cpu boost would not switch off (0x%x)", static_cast<unsigned>(rc));
        }
        return;
    }
    active = on;
}

bool core_busy(float busy[kCoreCount]) {
    // `svcGetInfo(InfoType_IdleTickCount)` answers only for the caller's own core, so the thread
    // visits each core in turn (pinned, yielding until moved) and returns to the one it started on.
    static u64 last_idle[kCoreCount] = {};
    static u64 last_tick[kCoreCount] = {};
    static bool sampled = false;
    static bool refused = false;
    if (refused) return false;

    s32 home_core = 0;
    u64 home_mask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&home_core, &home_mask, CUR_THREAD_HANDLE))) return false;

    u64 idle[kCoreCount] = {};
    u64 tick[kCoreCount] = {};
    bool complete = true;
    for (int core = 0; core < kCoreCount && complete; ++core) {
        if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core))) {
            complete = false;
            break;
        }
        // The move takes effect at the next reschedule; a yield is one.
        for (int tries = 0; tries < 64 && svcGetCurrentProcessorNumber() != static_cast<u32>(core);
             ++tries) {
            svcSleepThread(0);
        }
        if (svcGetCurrentProcessorNumber() != static_cast<u32>(core) ||
            R_FAILED(svcGetInfo(&idle[core], InfoType_IdleTickCount, INVALID_HANDLE,
                                static_cast<u64>(-1)))) {
            complete = false;
            break;
        }
        tick[core] = armGetSystemTick();
    }
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, home_core, static_cast<u32>(home_mask));
    for (int tries = 0; tries < 64 && svcGetCurrentProcessorNumber() != static_cast<u32>(home_core);
         ++tries) {
        svcSleepThread(0);
    }
    if (!complete) {
        refused = true;
        TL_LOG_INFO("core load: the kernel would not report every core's idle time; not logged");
        return false;
    }

    const bool had = sampled;
    for (int core = 0; core < kCoreCount; ++core) {
        if (had) {
            const u64 elapsed = tick[core] - last_tick[core];
            const u64 idled = idle[core] - last_idle[core];
            const float fraction =
                elapsed > 0 ? 1.0f - static_cast<float>(idled) / static_cast<float>(elapsed) : 0.0f;
            busy[core] = fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;
        }
        last_idle[core] = idle[core];
        last_tick[core] = tick[core];
    }
    sampled = true;
    return had;
}

bool clock_rates(unsigned& cpu_mhz, unsigned& gpu_mhz, unsigned& memory_mhz) {
    // `clkrst` from 8.0.0, `pcv` before it: the rate the clock actually runs at (what an
    // overclocking sysmodule changes). Opened once and kept; the process's end closes them.
    static bool tried = false;
    static bool usable = false;
    static bool modern = false;
    static ClkrstSession sessions[3];
    if (!tried) {
        tried = true;
        // Only when the card asks (sdmc:/switch/torchlight/clocks): these calls have taken the
        // console down mid-game.
        std::FILE* asked = std::fopen("sdmc:/switch/torchlight/clocks", "rb");
        if (asked == nullptr) return false;
        std::fclose(asked);
        modern = hosversionAtLeast(8, 0, 0);
        Result rc = 0;
        if (modern) {
            rc = clkrstInitialize();
            const PcvModuleId ids[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
            for (int i = 0; i < 3 && R_SUCCEEDED(rc); ++i)
                rc = clkrstOpenSession(&sessions[i], ids[i], 3);
        } else {
            rc = pcvInitialize();
        }
        usable = R_SUCCEEDED(rc);
        if (!usable) {
            TL_LOG_INFO("clocks: the clock service refused the process (0x%x); not logged",
                        static_cast<unsigned>(rc));
        }
    }
    if (!usable) return false;

    u32 hz[3] = {};
    const PcvModule modules[3] = {PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC};
    for (int i = 0; i < 3; ++i) {
        const Result rc =
            modern ? clkrstGetClockRate(&sessions[i], &hz[i]) : pcvGetClockRate(modules[i], &hz[i]);
        if (R_FAILED(rc)) return false;
    }
    cpu_mhz = hz[0] / 1000000u;
    gpu_mhz = hz[1] / 1000000u;
    memory_mhz = hz[2] / 1000000u;
    return true;
}

// The first worker shares core 2 with the mixer and the log writer, at the game thread's priority:
// the mixer keeps 85 ms of sound queued and waits out a fork-join phase. The rest go to core 1,
// below Mesa's glthread (libnx's default for a new thread, 0x3B), so a worker never makes the
// driver's thread wait.
void prepare_worker_thread(unsigned index) {
    constexpr s32 kFirstCore = 2;
    constexpr s32 kFirstPriority = 0x2C;
    constexpr s32 kOtherCore = 1;
    constexpr s32 kOtherPriority = 0x3C;
    const s32 core = index == 0 ? kFirstCore : kOtherCore;
    const Result placed = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    const Result raised =
        svcSetThreadPriority(CUR_THREAD_HANDLE, index == 0 ? kFirstPriority : kOtherPriority);
    TL_LOG_INFO("jobs: worker %u on core %u (0x%x, 0x%x)", index, svcGetCurrentProcessorNumber(),
                static_cast<unsigned>(placed), static_cast<unsigned>(raised));
}

}  // namespace tl::core
