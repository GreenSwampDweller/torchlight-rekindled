// Switch 2 only: what the program may ask of the console, enforced at the calls every request
// passes through. Under kaiHEN a homebrew may open any system service and the console's NAND;
// nothing but the program itself keeps it off them. Linked with --wrap (cmake/switch2_guard.cmake),
// so every caller in the executable goes through these: the port, OGRE, libounce, OunceVK.
//
// - Services: a request for one that writes the console's storage, settings or firmware state,
//   installs or removes software, files error or play reports, or drives hardware directly is
//   refused, whoever asks. The names asked for are kept for the log.
// - Writes: creating, deleting, renaming, or opening a file for writing only on the SD card's own
//   file system, and only inside a /switch/torchlight*/ folder. No recursive delete at all.
// - Mounts: no device but the SD card.
#if defined(__SWITCH2__)

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// Families refused by name: an entry matches the name itself, or the name followed by ':' and
// anything (so "ns" covers "ns:am2"); an entry ending in ':' or with a ':' inside matches as a
// prefix of its own.
constexpr const char* kRefused[] = {
    // Storage, content, installation, tickets, the loader and process control.
    "fsp-pr",
    "fsp-ldr",
    "ns",
    "nsd",
    "ncm",
    "lr",
    "es",
    "nim",
    "aoc",
    "arp:w",
    "pm",
    "ldr",
    "ro",
    "sm:m",
    "dmnt",
    "pgl",
    "mig",
    // Security processor, fuses, power and reboot, the real-time clock, sleep.
    "spl",
    "bpc",
    "spsm",
    "psc",
    "omm",
    // Settings that are written: factory and calibration, the system clock.
    "set:fd",
    "set:cal",
    "time:a",
    "time:su",
    "time:s",
    // Error, crash and play reports, and what uploads them.
    "fatal",
    "erpt",
    "eupld",
    "prepo",
    "srepo",
    "sprof",
    "ectx",
    "olsc",
    "bcat",
    "npns",
    "news",
    "notif",
    "ovln",
    // Accounts, friends, parental controls, the album, the Mii database, amiibo.
    "acc",
    "friend",
    "pctl",
    "caps",
    "mii",
    "miiimg",
    "nfc",
    "nfp",
    // Hardware and radios driven directly.
    "i2c",
    "gpio",
    "pinmux",
    "pwm",
    "uart",
    "tc",
    "ts",
    "lbl",
    "audctl",
    "hid:sys",
    "hid:dbg",
    "hidbus",
    "xcd:sys",
    "btm",
    "btdrv",
    "bt",
    "usb",
    "wlan",
    "ldn",
    "nifm",
    "ssl",
    "apm:sys",
    "htc",
    "htcs",
};

constexpr Result kServiceRefused = MAKERESULT(Module_Libnx, LibnxError_NotFound);
constexpr Result kWriteRefused = MAKERESULT(Module_Libnx, LibnxError_IoError);

Mutex g_lock;
char g_asked[48][9];
unsigned g_asked_count;
char g_refused[24][112];
unsigned g_refused_count;  // every refusal, also past the ones kept

void name_of(SmServiceName name, char out[9]) {
    std::memcpy(out, name.name, 8);
    out[8] = '\0';
}

bool refused_service(const char* name) {
    for (const char* entry : kRefused) {
        const size_t n = std::strlen(entry);
        if (std::strncmp(name, entry, n) != 0) continue;
        if (name[n] == '\0' || name[n] == ':' || entry[n - 1] == ':' || std::strchr(entry, ':'))
            return true;
    }
    return false;
}

void note_asked(const char* name) {
    mutexLock(&g_lock);
    bool seen = false;
    for (unsigned i = 0; i < g_asked_count && !seen; ++i) seen = std::strcmp(g_asked[i], name) == 0;
    if (!seen && g_asked_count < sizeof g_asked / sizeof g_asked[0])
        std::memcpy(g_asked[g_asked_count++], name, 9);
    mutexUnlock(&g_lock);
}

void note_refused(const char* format, ...) {
    mutexLock(&g_lock);
    if (g_refused_count < sizeof g_refused / sizeof g_refused[0]) {
        va_list args;
        va_start(args, format);
        std::vsnprintf(g_refused[g_refused_count], sizeof g_refused[0], format, args);
        va_end(args);
    }
    ++g_refused_count;
    mutexUnlock(&g_lock);
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// The SD card's own file system, as the program mounted it: a write anywhere else is refused.
bool on_sd_card(const FsFileSystem* fs) {
    const FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
    return fs != nullptr && sd != nullptr && fs->s.session == sd->s.session &&
           fs->s.object_id == sd->s.object_id;
}

}  // namespace

extern "C" {

// A path on the SD card the program may change: inside /switch/torchlight*/ (the folder's name
// may go on, `torchlight-xbox`), case aside, with no `..`, `\` or `:` that could lead out of it.
bool tl_switch2_write_allowed(const char* path) {
    if (path == nullptr) return false;
    if (std::strncmp(path, "sdmc:", 5) == 0) path += 5;
    if (std::strstr(path, "..") != nullptr || std::strchr(path, '\\') != nullptr ||
        std::strchr(path, ':') != nullptr) {
        return false;
    }
    static const char kRoot[] = "/switch/torchlight";
    for (size_t i = 0; i + 1 < sizeof kRoot; ++i) {
        if (lower(path[i]) != kRoot[i]) return false;
    }
    const char* rest = path + sizeof kRoot - 1;
    while ((*rest >= 'a' && *rest <= 'z') || (*rest >= 'A' && *rest <= 'Z') ||
           (*rest >= '0' && *rest <= '9') || *rest == '-' || *rest == '_') {
        ++rest;
    }
    return *rest == '\0' || *rest == '/';
}

// What was asked and refused, for the log: one line, then one line per refusal kept.
size_t tl_switch2_guard_summary(char* out, size_t size) {
    if (out == nullptr || size == 0) return 0;
    mutexLock(&g_lock);
    size_t used =
        static_cast<size_t>(std::snprintf(out, size, "guard: %u service(s) asked:", g_asked_count));
    for (unsigned i = 0; i < g_asked_count && used < size; ++i)
        used += static_cast<size_t>(std::snprintf(out + used, size - used, " %s", g_asked[i]));
    if (used < size)
        used += static_cast<size_t>(
            std::snprintf(out + used, size - used, "; %u refused", g_refused_count));
    const unsigned kept = g_refused_count < 24 ? g_refused_count : 24;
    for (unsigned i = 0; i < kept && used < size; ++i)
        used += static_cast<size_t>(
            std::snprintf(out + used, size - used, "\nguard: refused %s", g_refused[i]));
    mutexUnlock(&g_lock);
    return used < size ? used : size - 1;
}

// --- services ---------------------------------------------------------------------------------

Result __real_smGetServiceWrapper(Service* service_out, SmServiceName name);
Result __real_smGetServiceOriginal(Handle* handle_out, SmServiceName name);

Result __wrap_smGetServiceWrapper(Service* service_out, SmServiceName name) {
    char text[9];
    name_of(name, text);
    note_asked(text);
    if (refused_service(text)) {
        note_refused("service %s", text);
        return kServiceRefused;
    }
    return __real_smGetServiceWrapper(service_out, name);
}

Result __wrap_smGetServiceOriginal(Handle* handle_out, SmServiceName name) {
    char text[9];
    name_of(name, text);
    note_asked(text);
    if (refused_service(text)) {
        note_refused("service %s", text);
        return kServiceRefused;
    }
    return __real_smGetServiceOriginal(handle_out, name);
}

// --- mounts -----------------------------------------------------------------------------------

int __real_fsdevMountDevice(const char* name, FsFileSystem fs);

int __wrap_fsdevMountDevice(const char* name, FsFileSystem fs) {
    if (name == nullptr || std::strcmp(name, "sdmc") != 0) {
        note_refused("mount %s", name != nullptr ? name : "(null)");
        return -1;
    }
    return __real_fsdevMountDevice(name, fs);
}

// --- writes -----------------------------------------------------------------------------------

Result __real_fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 option);
Result __real_fsFsDeleteFile(FsFileSystem* fs, const char* path);
Result __real_fsFsCreateDirectory(FsFileSystem* fs, const char* path);
Result __real_fsFsDeleteDirectory(FsFileSystem* fs, const char* path);
Result __real_fsFsRenameFile(FsFileSystem* fs, const char* cur_path, const char* new_path);
Result __real_fsFsRenameDirectory(FsFileSystem* fs, const char* cur_path, const char* new_path);
Result __real_fsFsOpenFile(FsFileSystem* fs, const char* path, u32 mode, FsFile* out);

namespace {
bool may_write(const char* what, FsFileSystem* fs, const char* path) {
    if (on_sd_card(fs) && tl_switch2_write_allowed(path)) return true;
    note_refused("%s %s%s", what, on_sd_card(fs) ? "" : "(not the SD card) ",
                 path ? path : "(null)");
    return false;
}
}  // namespace

Result __wrap_fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 option) {
    if (!may_write("create", fs, path)) return kWriteRefused;
    return __real_fsFsCreateFile(fs, path, size, option);
}

Result __wrap_fsFsDeleteFile(FsFileSystem* fs, const char* path) {
    if (!may_write("delete", fs, path)) return kWriteRefused;
    return __real_fsFsDeleteFile(fs, path);
}

// `/switch` itself may be made: the folder every homebrew's lives in, and the probe's first step.
Result __wrap_fsFsCreateDirectory(FsFileSystem* fs, const char* path) {
    const bool switch_folder =
        on_sd_card(fs) && path != nullptr &&
        (std::strcmp(path, "/switch") == 0 || std::strcmp(path, "/switch/") == 0);
    if (!switch_folder && !may_write("mkdir", fs, path)) return kWriteRefused;
    return __real_fsFsCreateDirectory(fs, path);
}

Result __wrap_fsFsDeleteDirectory(FsFileSystem* fs, const char* path) {
    if (!may_write("rmdir", fs, path)) return kWriteRefused;
    return __real_fsFsDeleteDirectory(fs, path);
}

Result __wrap_fsFsDeleteDirectoryRecursively(FsFileSystem* fs, const char* path) {
    (void)fs;
    note_refused("recursive delete %s", path ? path : "(null)");
    return kWriteRefused;
}

Result __wrap_fsFsCleanDirectoryRecursively(FsFileSystem* fs, const char* path) {
    (void)fs;
    note_refused("recursive clean %s", path ? path : "(null)");
    return kWriteRefused;
}

Result __wrap_fsFsRenameFile(FsFileSystem* fs, const char* cur_path, const char* new_path) {
    if (!may_write("rename from", fs, cur_path) || !may_write("rename to", fs, new_path))
        return kWriteRefused;
    return __real_fsFsRenameFile(fs, cur_path, new_path);
}

Result __wrap_fsFsRenameDirectory(FsFileSystem* fs, const char* cur_path, const char* new_path) {
    if (!may_write("rename from", fs, cur_path) || !may_write("rename to", fs, new_path))
        return kWriteRefused;
    return __real_fsFsRenameDirectory(fs, cur_path, new_path);
}

// Reading is open anywhere; writing or appending only where a write may go.
Result __wrap_fsFsOpenFile(FsFileSystem* fs, const char* path, u32 mode, FsFile* out) {
    if ((mode & (FsOpenMode_Write | FsOpenMode_Append)) != 0 &&
        !may_write("open for writing", fs, path))
        return kWriteRefused;
    return __real_fsFsOpenFile(fs, path, mode, out);
}

// Never needed, and not linked in: the real one is a general file-system query.
Result __wrap_fsFsSetConcatenationFileAttribute(FsFileSystem* fs, const char* path) {
    (void)fs;
    note_refused("set archive bit %s", path ? path : "(null)");
    return kWriteRefused;
}

}  // extern "C"

#endif  // __SWITCH2__
