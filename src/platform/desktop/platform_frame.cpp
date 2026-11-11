// Desktop event pump. OGRE creates the window and its WindowProc but nothing dispatches to it;
// without this the window stops responding.
#include "core/platform.h"

#include "core/log.h"
#include "platform/desktop/typed_keys.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(__APPLE__) && !defined(TL_PLATFORM_IOS)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX  // libstdc++'s <string> defines it on MinGW
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(TL_PLATFORM_MACOS)
extern "C" void tl_macos_error_message(const char* summary, const char* detail);
extern "C" bool tl_macos_should_keep_running();
#endif

namespace tl::core {

// The working directory: `--game-data game_data`, `torchlight.log`, `save/`.
const std::string& platform_data_root() {
#if defined(TL_PLATFORM_MACOS)
    static const std::string root = [] {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0') return std::string();
        std::filesystem::path path(home);
        path /= "Library/Application Support/Torchlight Rekindled";
        std::error_code error;
        std::filesystem::create_directories(path, error);
        if (error) return std::string();
        return path.string() + "/";
    }();
    return root;
#else
    static const std::string root;
    return root;
#endif
}

std::string platform_program_folder() {
    std::string path;
#if defined(_WIN32)
    char buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    path.assign(buffer, length);
#elif defined(__APPLE__)
    uint32_t length = 0;
    (void)_NSGetExecutablePath(nullptr, &length);
    if (length == 0) return {};
    std::string buffer(length, '\0');
    if (_NSGetExecutablePath(buffer.data(), &length) != 0) return {};
    path.assign(buffer.c_str());
#else
    char buffer[4096] = {};
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (length <= 0) return {};
    path.assign(buffer, static_cast<size_t>(length));
#endif
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

#if !defined(TL_PLATFORM_MACOS)
// OGRE makes the window itself on other desktop platforms.
void platform_window_params(std::map<std::string, std::string>& params) {
    (void)params;
#if defined(_WIN32)
    // DPI-aware before the window exists. Unaware, a display scaled past 100% takes the
    // window's size in scaled units: a fullscreen window comes out larger than the screen
    // and shows its top-left part, a windowed one is drawn small and stretched. Per-monitor
    // (Windows 10 1703) where the system has it, else system-wide.
    using SetContext = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    FARPROC found =
        user32 != nullptr ? GetProcAddress(user32, "SetProcessDpiAwarenessContext") : nullptr;
    // Through `void (*)()`, the type GCC takes as any function's.
    const auto set_context = reinterpret_cast<SetContext>(reinterpret_cast<void (*)()>(found));
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, a handle Windows defines as -4.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const per_monitor_v2 = reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4));
    if (set_context == nullptr || set_context(per_monitor_v2) == 0) SetProcessDPIAware();
#endif
}
#endif

// In the log and on stderr, and on Windows in a message box: a player who
// double-clicked the program has no terminal to read.
void platform_error_message(const std::string& summary, const std::string& detail) {
    TL_LOG_ERROR("%s", summary.c_str());
    std::fprintf(stderr, "%s\n\n%s\n", summary.c_str(), detail.c_str());
#if defined(_WIN32)
    const std::string text = summary + "\n\n" + detail;
    MessageBoxA(nullptr, text.c_str(), "Torchlight Rekindled", MB_OK | MB_ICONERROR);
#elif defined(TL_PLATFORM_MACOS)
    tl_macos_error_message(summary.c_str(), detail.c_str());
#endif
}

#if !defined(TL_PLATFORM_MACOS)
void pump_events() {
    note_events_pumped();
#if defined(_WIN32)
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        if (msg.message == WM_CHAR) note_typed_unit(static_cast<unsigned>(msg.wParam));
        if (msg.message == WM_MOUSEWHEEL) note_wheel(GET_WHEEL_DELTA_WPARAM(msg.wParam));
        DispatchMessageW(&msg);
    }
#endif
    // X11 and Cocoa are pumped inside OGRE's own GLX/Cocoa window code, so
    // there is nothing to do on those platforms.
}
#endif

// A desktop's display rate is the player's (vsync follows it).
void platform_set_frame_rate(float fps) { (void)fps; }

void platform_sleep_until(std::chrono::steady_clock::time_point deadline) {
#if defined(_WIN32)
    // A plain sleep wakes on the system tick, 15.6 ms unless something raised it: too coarse
    // to hold 60 frames a second. A high-resolution waitable timer (Windows 10 1803) is not.
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */,
                                                 TIMER_ALL_ACCESS);
    const auto left = deadline - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return;
    if (timer != nullptr) {
        LARGE_INTEGER due;
        // Relative, in 100 ns units.
        due.QuadPart = -static_cast<LONGLONG>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != 0) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_until(deadline);
}

bool should_keep_running() {
    // Desktop has no external lifecycle; the window closing ends the loop.
#if defined(TL_PLATFORM_MACOS)
    return tl_macos_should_keep_running();
#else
    return true;
#endif
}

bool memory_usage(unsigned long long& used, unsigned long long& total) {
    // A desktop process has no meaningful ceiling; no number rather than a meaningless one.
    (void)used;
    (void)total;
    return false;
}

void set_loading_boost(bool on) {
    // The desktop's clocks are the operating system's business.
    (void)on;
}

bool core_busy(float busy[kCoreCount]) {
    // Scheduled by the operating system across however many cores it has;
    // nothing here decides which, so there is nothing to report.
    (void)busy;
    return false;
}

bool clock_rates(unsigned& cpu_mhz, unsigned& gpu_mhz, unsigned& memory_mhz) {
    (void)cpu_mhz;
    (void)gpu_mhz;
    (void)memory_mhz;
    return false;
}

// Scheduled by the operating system across however many cores it has.
void prepare_worker_thread(unsigned index) { (void)index; }

void start_gpu_waits() {}
bool gpu_wait_times(GpuWaitTimes& game, GpuWaitTimes& others) {
    (void)game;
    (void)others;
    return false;
}
const char* gpu_wait_name(int kind) {
    (void)kind;
    return "";
}

}  // namespace tl::core
