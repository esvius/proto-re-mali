// proto-re host: loaded into the game as binkw32.dll (a forwarding proxy, see binkw32.def).
// prototypeenginef.dll imports binkw32.dll, so by the time this DllMain runs the engine image is mapped and
// relocated; its own initialisation (EngineInitialize) has not run yet, so hooks are in place before any
// engine code executes.
#include <windows.h>

#include "difftest.h"
#include "hooks.h"
#include "probes.h"
#include "registry.h"

#include "../recomp/bridge.h"

namespace {

bool EngineBuildMatches() {
    auto* base = static_cast<uint8_t*>(protore::EngineAddress(0));
    if (!base) {
        protore::Log("prototypeenginef.dll is not loaded");
        return false;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    uint32_t stamp = nt->FileHeader.TimeDateStamp;
    protore::Log("engine loaded at %p, TimeDateStamp 0x%08x", base, stamp);
    if (stamp != protore::kEngineTimeDateStamp) {
        protore::Log("unsupported engine build (expected 0x%08x), no hooks installed", protore::kEngineTimeDateStamp);
        return false;
    }
    return true;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        protore::Log("proto-re host attached");
        // PROTORE_NOHOOKS=1 runs the original engine through the same proxy, for A/B comparisons.
        if (GetEnvironmentVariableA("PROTORE_NOHOOKS", nullptr, 0) != 0) {
            protore::Log("PROTORE_NOHOOKS set, hooks disabled");
        } else if (EngineBuildMatches()) {
            size_t count = 0;
            protore::Hook* hooks = protore::Registry(&count);
            char recomp_mode[8] = "";
            GetEnvironmentVariableA("PROTORE_RECOMP", recomp_mode, sizeof(recomp_mode));
            if (lstrcmpA(recomp_mode, "all") == 0) {
                // Whole engine: every enterable function runs its recompiled version; no hand hooks.
                protore::InstallAllRecompEntries();
            } else {
                int installed = protore::InstallHooks(hooks, count);
                protore::Log("%d of %u hooks installed", installed, static_cast<unsigned>(count));
            }
            if (GetEnvironmentVariableA("PROTORE_DIFFTEST", nullptr, 0) != 0) {
                protore::InstallDiffTestTrigger();
            }
            protore::InstallProbes();
        }
    }
    return TRUE;
}
