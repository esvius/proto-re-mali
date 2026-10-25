#include "hooks.h"

#include "../recomp/bridge.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace protore {

namespace {

HMODULE g_engine = nullptr;
FILE* g_log = nullptr;

Hook* g_hooks = nullptr;
size_t g_hook_count = 0;

HMODULE Engine() {
    if (!g_engine) {
        g_engine = GetModuleHandleA("prototypeenginef.dll");
    }
    return g_engine;
}

// Bump allocator for trampolines in one executable page block.
uint8_t* AllocTrampoline(size_t size) {
    static uint8_t* pool = nullptr;
    static size_t used = 0;
    constexpr size_t kPoolSize = 64 * 1024;
    if (!pool) {
        pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPoolSize, MEM_COMMIT | MEM_RESERVE,
                                                  PAGE_EXECUTE_READWRITE));
        if (!pool) {
            return nullptr;
        }
    }
    size = (size + 15) & ~size_t(15);
    if (used + size > kPoolSize) {
        return nullptr;
    }
    uint8_t* p = pool + used;
    used += size;
    return p;
}

// Encodes JMP rel32 into `out`; the displacement is relative to `address`, where the jump will execute
// (which differs from `out` when the bytes are assembled in a buffer before being copied into place).
void WriteJump(uint8_t* out, const void* address, const void* to) {
    int32_t rel =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (reinterpret_cast<uintptr_t>(address) + 5));
    out[0] = 0xE9;
    memcpy(out + 1, &rel, sizeof(rel));
}

bool WriteCode(uint8_t* target, const uint8_t* bytes, size_t len) {
    DWORD old_protect;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    memcpy(target, bytes, len);
    VirtualProtect(target, len, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    return true;
}

}  // namespace

void Log(const char* fmt, ...) {
    if (!g_log) {
        // Append, and tag every line with the process id: the game may run more than one process.
        g_log = fopen("proto-re.log", "a");
        if (!g_log) {
            return;
        }
    }
    fprintf(g_log, "[%lu] ", GetCurrentProcessId());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

void* EngineAddress(uint32_t rva) {
    HMODULE engine = Engine();
    return engine ? reinterpret_cast<uint8_t*>(engine) + rva : nullptr;
}

void* OriginalFunction(uint32_t rva) {
    for (size_t i = 0; i < g_hook_count; ++i) {
        if (g_hooks[i].rva == rva && g_hooks[i].detour.trampoline) {
            return g_hooks[i].detour.trampoline;
        }
    }
    return EngineAddress(rva);
}

bool DetourAttach(Detour* d, uint32_t rva, size_t stolen_len, const uint8_t* expected, size_t expected_len,
                  void* replacement, int rel32_at) {
    auto* target = static_cast<uint8_t*>(EngineAddress(rva));
    if (!target || stolen_len < 5 || stolen_len > sizeof(d->saved) || expected_len < stolen_len) {
        return false;
    }
    if (memcmp(target, expected, expected_len) != 0) {
        return false;
    }
    if (rel32_at >= 0 && (static_cast<size_t>(rel32_at) + 5 > stolen_len ||
                          (target[rel32_at] != 0xE8 && target[rel32_at] != 0xE9))) {
        return false;
    }
    uint8_t* tramp = AllocTrampoline(stolen_len + 5);
    if (!tramp) {
        return false;
    }
    memcpy(tramp, target, stolen_len);
    if (rel32_at >= 0) {
        int32_t rel;
        memcpy(&rel, target + rel32_at + 1, sizeof(rel));
        const uint8_t* destination = target + rel32_at + 5 + rel;
        int32_t moved = static_cast<int32_t>(reinterpret_cast<uintptr_t>(destination) -
                                             reinterpret_cast<uintptr_t>(tramp + rel32_at + 5));
        memcpy(tramp + rel32_at + 1, &moved, sizeof(moved));
    }
    WriteJump(tramp + stolen_len, tramp + stolen_len, target + stolen_len);

    // PROTORE_NULLPATCH=1 performs the same protect/write/restore sequence but writes back the original
    // bytes, to separate effects of touching the code page from effects of the redirected function.
    uint8_t patch[16];
    memcpy(patch, target, stolen_len);
    if (GetEnvironmentVariableA("PROTORE_NULLPATCH", nullptr, 0) == 0) {
        WriteJump(patch, target, replacement);
        for (size_t i = 5; i < stolen_len; ++i) {
            patch[i] = 0xCC;  // int3 over the tail of a split instruction: jumping there must fault loudly
        }
    }
    memcpy(d->saved, target, stolen_len);
    if (!WriteCode(target, patch, stolen_len)) {
        return false;
    }
    d->target = target;
    d->stolen_len = stolen_len;
    d->trampoline = tramp;
    return true;
}

void DetourDetach(Detour* d) {
    if (d->target) {
        WriteCode(d->target, d->saved, d->stolen_len);
        d->target = nullptr;
    }
}

namespace {

// Engine addresses of active test detours (tests nest only a handful).
constexpr size_t kMaxTestDetours = 64;
uint32_t g_test_detours[kMaxTestDetours];
size_t g_test_detour_count = 0;

}  // namespace

}  // namespace protore

extern "C" volatile uint32_t RecompTestDetours;

namespace protore {

bool TestDetourActive(uint32_t va) {
    for (size_t i = 0; i < g_test_detour_count; ++i) {
        if (g_test_detours[i] == va) {
            return true;
        }
    }
    return false;
}

TestDetour::TestDetour(uint32_t rva, size_t stolen_len, const uint8_t* expected, size_t expected_len,
                       void* replacement, int rel32_at) {
    rva_ = rva;
    ++RecompTestDetours;
    if (g_test_detour_count < kMaxTestDetours) {
        g_test_detours[g_test_detour_count++] = static_cast<uint32_t>(kEngineImageBase) + rva;
    }
    for (size_t i = 0; i < g_hook_count; ++i) {
        if (g_hooks[i].rva == rva && g_hooks[i].detour.target) {
            suspended_ = &g_hooks[i];
            DetourDetach(&suspended_->detour);
            break;
        }
    }
    if (!DetourAttach(&detour_, rva, stolen_len, expected, expected_len, replacement, rel32_at)) {
        Log("TestDetour: could not detour rva 0x%08x", rva);
    }
}

TestDetour::~TestDetour() {
    DetourDetach(&detour_);
    --RecompTestDetours;
    for (size_t i = g_test_detour_count; i-- > 0;) {
        if (g_test_detours[i] == static_cast<uint32_t>(kEngineImageBase) + rva_) {
            g_test_detours[i] = g_test_detours[--g_test_detour_count];
            break;
        }
    }
    if (suspended_) {
        Hook& h = *suspended_;
        if (!DetourAttach(&h.detour, h.rva, h.stolen_len, h.expected, h.expected_len, h.replacement, h.rel32_at)) {
            Log("TestDetour: could not restore hook %s", h.name);
        }
    }
}

namespace {

// Overwrites the start of one of our own functions with JMP rel32 to `to`. Functions are 16-byte aligned
// (-falign-functions=16); the five bytes must stay inside that alignment block or the function itself.
bool RedirectHandFunction(void* from, void* to) {
    auto* p = static_cast<uint8_t*>(from);
    uint8_t jump[5];
    WriteJump(jump, p, to);
    return WriteCode(p, jump, sizeof(jump));
}

}  // namespace

int InstallHooks(Hook* hooks, size_t count) {
    g_hooks = hooks;
    g_hook_count = count;
    // PROTORE_ONLY=<name> installs just that hook, for bisecting behaviour changes to one function.
    char only[128] = "";
    GetEnvironmentVariableA("PROTORE_ONLY", only, sizeof(only));
    // PROTORE_SKIP=<prefix>[,<prefix>...] leaves every hook whose name starts with one of the prefixes uninstalled.
    char skip[512] = "";
    GetEnvironmentVariableA("PROTORE_SKIP", skip, sizeof(skip));
    auto skipped = [&skip](const char* name) {
        for (const char* p = skip; *p;) {
            const char* end = strchr(p, ',');
            size_t len = end ? static_cast<size_t>(end - p) : strlen(p);
            if (len && strncmp(name, p, len) == 0) {
                return true;
            }
            p += len + (end ? 1 : 0);
        }
        return false;
    };
    const bool redirect_hand = GetEnvironmentVariableA("PROTORE_DIFFTEST_RECOMP", nullptr, 0) != 0;
    const bool recomp = redirect_hand || GetEnvironmentVariableA("PROTORE_RECOMP", nullptr, 0) != 0;
    int recompiled = 0;
    int redirected = 0;
    int installed = 0;
    for (size_t i = 0; i < count; ++i) {
        Hook& h = hooks[i];
        if ((only[0] && strcmp(only, h.name) != 0) || skipped(h.name)) {
            continue;
        }
        // PROTORE_RECOMP=1 runs the statically recompiled version instead of the hand reconstruction when one
        // exists for this function (also for hand reconstructions that are still disabled).
        void* stub = recomp ? RecompEntryStub(static_cast<uint32_t>(kEngineImageBase) + h.rva) : nullptr;
        if (!h.enabled && !stub) {
            continue;
        }
        if (stub) {
            // PROTORE_DIFFTEST_RECOMP=1: the hand reconstruction itself jumps to the recompiled code, so every
            // differential test compares the original against the recompiled version.
            if (redirect_hand && h.replacement) {
                if (RedirectHandFunction(h.replacement, stub)) {
                    ++redirected;
                } else {
                    Log("recomp: could not redirect hand reconstruction of %s", h.name);
                }
            }
            h.replacement = stub;
            ++recompiled;
        }
        if (!DetourAttach(&h.detour, h.rva, h.stolen_len, h.expected, h.expected_len, h.replacement, h.rel32_at)) {
            Log("hook %s: not installed at rva 0x%08x (bytes mismatch or protect failure)", h.name, h.rva);
            continue;
        }
        Log("hook %s: installed at rva 0x%08x", h.name, h.rva);
        ++installed;
    }
    if (recomp) {
        Log("recomp: %d hooks run recompiled code", recompiled);
    }
    if (redirect_hand) {
        Log("recomp: %d hand reconstructions redirected to recompiled code", redirected);
    }
    return installed;
}

}  // namespace protore
