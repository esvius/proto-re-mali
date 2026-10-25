// Hook-and-replace infrastructure: each reconstructed function is registered with the RVA of the original
// in prototypeenginef.dll and the original's first bytes, so a build mismatch is detected instead of
// silently patching the wrong code. Every hook also builds a trampoline, so the original stays callable
// for differential tests.
#pragma once

#include <cstddef>
#include <cstdint>

namespace protore {

// Engine build this table was made for (PE TimeDateStamp of prototypeenginef.dll, Steam build 19788432).
constexpr uint32_t kEngineTimeDateStamp = 0x689E2026;
// Preferred image base; Ghidra addresses are kEngineImageBase + RVA.
constexpr uintptr_t kEngineImageBase = 0x10000000;

// A JMP rel32 placed over the first `stolen_len` bytes of an engine function. stolen_len must end on an
// instruction boundary, be >= 5, and the stolen instructions must be position independent (checked by hand
// against the disassembly when an entry is added).
struct Detour {
    uint8_t* target = nullptr;
    uint8_t saved[16] = {};
    size_t stolen_len = 0;
    void* trampoline = nullptr;  // stolen bytes + JMP back to target + stolen_len
};

// `rel32_at`: offset inside the stolen bytes of a CALL/JMP rel32 (E8/E9) whose displacement must be re-based
// when copied into the trampoline; -1 when the stolen instructions contain none.
bool DetourAttach(Detour* d, uint32_t rva, size_t stolen_len, const uint8_t* expected, size_t expected_len,
                  void* replacement, int rel32_at = -1);
void DetourDetach(Detour* d);

struct Hook {
    const char* name;        // reconstructed symbol, e.g. "core::Report"
    uint32_t rva;            // original function RVA (Ghidra address - kEngineImageBase)
    void* replacement;       // reconstructed implementation
    uint8_t expected[16];    // original first bytes, checked before patching
    size_t expected_len;
    size_t stolen_len;       // bytes moved into the trampoline (instruction boundary, >= 5)
    bool enabled;            // disabled hooks stay in the table for differential testing
    Detour detour;           // filled by InstallHooks
    int rel32_at = -1;       // see DetourAttach
};

// Returns pointer to a location in the loaded engine. For a function this is its live entry point: calls through
// it reach whatever is installed there (original, reconstruction hook, or a test detour), so reconstructions call
// other engine functions this way while the engine is hybrid.
void* EngineAddress(uint32_t rva);

// Callable original for `rva`: the trampoline if that function is hooked, else the engine address.
void* OriginalFunction(uint32_t rva);

// Patches every enabled hook; returns number installed, logs each failure.
int InstallHooks(Hook* hooks, size_t count);

// True while a TestDetour replaces the function at engine address `va`; recompiled code then calls through the
// engine entry point instead of calling the recompiled function directly, so tests can observe the call.
bool TestDetourActive(uint32_t va);

// Test-only detour: for its lifetime the function at `rva` is replaced by `replacement`, even if a reconstruction
// hook is installed there (the hook is suspended and restored afterwards).
class TestDetour {
public:
    TestDetour(uint32_t rva, size_t stolen_len, const uint8_t* expected, size_t expected_len, void* replacement,
               int rel32_at = -1);
    ~TestDetour();
    TestDetour(const TestDetour&) = delete;
    TestDetour& operator=(const TestDetour&) = delete;
    bool ok() const { return detour_.target != nullptr; }

private:
    Hook* suspended_ = nullptr;
    Detour detour_;
    uint32_t rva_ = 0;
};

void Log(const char* fmt, ...);

}  // namespace protore
