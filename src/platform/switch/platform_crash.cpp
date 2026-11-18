// Switch 2 only: a crash ends here, not in the system's crash reporter. Without a handler of the
// program's own, a fault goes to the kernel's debug path, and the system files a crash report in
// its own storage (and sends it when online). This one writes `crash.txt` into the game's folder
// (the program counter, link register and fault address as offsets into the .nro, and the code
// addresses found on the stack) and ends the process.
#if defined(__SWITCH2__)

#include "core/platform.h"

#include <switch.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>

extern "C" {

// The handler runs on this stack; stdio and the SD card need more than libnx's 1 KiB.
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* ctx);
}

namespace {

// The program's code mapping: what an address is reported relative to.
MemoryInfo code_region() {
    MemoryInfo info{};
    u32 page = 0;
    (void)svcQueryMemory(&info, &page, reinterpret_cast<u64>(&__libnx_exception_handler));
    return info;
}

bool readable(u64 address) {
    MemoryInfo info{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&info, &page, address))) return false;
    return (info.perm & Perm_R) != 0 && address + 8 <= info.addr + info.size;
}

char g_text[4096];

}  // namespace

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    static bool once = false;
    if (once) svcExitProcess();  // a fault inside the handler
    once = true;

    const MemoryInfo code = code_region();
    const auto offset = [&code](u64 address) -> long long {
        return address >= code.addr && address < code.addr + code.size
                   ? static_cast<long long>(address - code.addr)
                   : -1;
    };
    int used = std::snprintf(
        g_text, sizeof g_text,
        "crash: desc 0x%x esr 0x%x\npc   +0x%llx (0x%llx)\nlr   +0x%llx (0x%llx)\n"
        "far  0x%llx\nsp   0x%llx\nstack code addresses:\n",
        ctx->error_desc, ctx->esr, offset(ctx->pc.x), static_cast<unsigned long long>(ctx->pc.x),
        offset(ctx->lr.x), static_cast<unsigned long long>(ctx->lr.x),
        static_cast<unsigned long long>(ctx->far.x), static_cast<unsigned long long>(ctx->sp.x));
    int found = 0;
    for (u64 at = ctx->sp.x; at < ctx->sp.x + 0x4000 && found < 48 && used > 0 &&
                             used < static_cast<int>(sizeof g_text) - 64;
         at += 8) {
        if (!readable(at)) break;
        const long long hit = offset(*reinterpret_cast<const u64*>(at));
        if (hit < 0) continue;
        used += std::snprintf(g_text + used, sizeof g_text - used, "  sp+0x%llx: +0x%llx\n",
                              static_cast<unsigned long long>(at - ctx->sp.x), hit);
        ++found;
    }

    // The game's own folder, inside the guard's; known since start-up, so nothing is allocated.
    char path[256];
    std::snprintf(path, sizeof path, "%scrash.txt", tl::core::platform_data_root().c_str());
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0 && used > 0) {
        (void)write(fd, g_text, static_cast<size_t>(used));
        fsync(fd);
        close(fd);
    }
    svcExitProcess();
}

#endif  // __SWITCH2__
