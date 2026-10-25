#include "probes.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "hooks.h"
#include "memwatch.h"

namespace protore {

namespace {

// core::CementLibrary vfunction8: opens a file inside an .rcf by its 32-bit name key (second stack argument).
// sub esp,10h; push ebx; mov ebx,[esp+1Ch]
constexpr uint32_t kOpenByKeyRva = 0x00650460;
constexpr uint8_t kOpenByKeyBytes[] = {0x83, 0xec, 0x10, 0x53, 0x8b, 0x5c, 0x24, 0x1c};

Detour g_open_by_key;

extern "C" void __stdcall LogOpenByKey(uint32_t return_address, uint32_t key) {
    Log("probe openbykey key=%08x caller_rva=%08x", key,
        return_address - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(EngineAddress(0))));
}

extern "C" {
void* g_open_by_key_trampoline = nullptr;  // read by the asm stub below
}

// Entry: [esp]=return address, [esp+4]=arg1, [esp+8]=key; ecx=this (thiscall). Preserves ecx and the stack,
// then continues in the original through its trampoline.
extern "C" __attribute__((naked)) void OpenByKeyProbe() {
    asm volatile(
        "pushl %ecx\n\t"         // [esp+4]=ret, [esp+12]=key
        "pushl 12(%esp)\n\t"     // key
        "pushl 8(%esp)\n\t"      // return address (shifted by the previous push)
        "call _LogOpenByKey@8\n\t"
        "popl %ecx\n\t"
        "jmp *_g_open_by_key_trampoline\n\t");
}

}  // namespace

void InstallProbes() {
    char probe[64] = "";
    if (GetEnvironmentVariableA("PROTORE_PROBE", probe, sizeof(probe)) == 0) {
        return;
    }
    if (strcmp(probe, "openbykey") == 0) {
        if (DetourAttach(&g_open_by_key, kOpenByKeyRva, sizeof(kOpenByKeyBytes), kOpenByKeyBytes,
                         sizeof(kOpenByKeyBytes), reinterpret_cast<void*>(&OpenByKeyProbe))) {
            g_open_by_key_trampoline = g_open_by_key.trampoline;
            Log("probe openbykey: installed");
        } else {
            Log("probe openbykey: could not install");
        }
    } else if (strcmp(probe, "watchdir") == 0) {
        StartRcfDirectoryWatch();
    }
}

}  // namespace protore
