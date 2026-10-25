// Native host (no Wine): runs the recompiled engine in a 64-bit process. Guest memory is a private 4 GiB window of
// the host address space at kGuestBase (RECOMP_FLAT_MEMORY, RECOMP_GUEST_BASE): a guest address is an offset into it.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

#include "../recomp/runtime.h"

namespace native {

// ---- logging ----
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// A start that cannot go on: logs, shows `what` to the user and ends the process (main.cpp).
[[noreturn]] void StartupFailure(const char* what);
[[noreturn]] void Fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- guest memory (memory.cpp) ----
// Reserves the guest address space. Must run before anything is mapped.
void InitGuestMemory();
// Page-granular allocation (VirtualAlloc). `at` != 0 requests a fixed address. Returns 0 on failure.
uint32_t PageAlloc(uint32_t size, uint32_t at = 0);
void PageFree(uint32_t addr);
// Byte-granular heap (malloc, HeapAlloc, operator new of the host-side shims).
uint32_t HeapAlloc(uint32_t size);
void HeapFree(uint32_t addr);
uint32_t HeapSize(uint32_t addr);
uint32_t HeapRealloc(uint32_t addr, uint32_t size);

// Guest address <-> host pointer. The guest's NULL stays nullptr (host code tests optional arguments for null).
constexpr uint64_t kGuestBase = RECOMP_GUEST_BASE;
template <typename T>
inline T* Ptr(uint32_t a) { return a ? reinterpret_cast<T*>(static_cast<uintptr_t>(kGuestBase + a)) : nullptr; }
inline char* Str(uint32_t a) { return Ptr<char>(a); }
inline uint32_t Addr(const void* p) {
    return p ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) - kGuestBase) : 0;
}
// Copies a host string into a new guest heap block.
uint32_t GuestStrdup(const char* s);

// ---- threads (thread.cpp) ----
// Per guest thread state: its TEB in guest memory and the Ctx of the code currently running on it.
struct GuestThread {
    uint32_t teb;
    uint32_t stack_lo, stack_hi;
    uint32_t id;
    uint32_t last_error;
    uint32_t current_fiber;  // guest fiber handle (0 when the thread is not a fiber)
};
GuestThread* CurrentThread();
// Creates the guest state for the calling host thread (main thread or a thread started by CreateThread).
GuestThread* AttachThread(uint32_t stack_size);
// Hang diagnostics (watchdog.cpp): guest threads register themselves; the watchdog watches the frame counter.
void RegisterGuestThread();

// ---- calls into guest code ----
// Calls the recompiled function at `va` with stdcall/cdecl arguments on the current guest thread's stack; returns
// eax (edx in *edx_out when given). ecx is passed for thiscall.
uint32_t GuestCall(uint32_t va, std::initializer_list<uint32_t> args, uint32_t ecx = 0, uint32_t* edx_out = nullptr);

// ---- imports (imports.cpp) ----
// An import handler runs with the guest return address at [esp] and the arguments above it.
using ImportFn = void (*)(Ctx*);
struct ImportDef {
    const char* dll;    // lower case, e.g. "kernel32.dll"
    const char* name;   // export name, or "#<ordinal>"
    ImportFn fn;
};
// Guest address that dispatches to the handler for dll!name (unimplemented ones log and abort when called).
uint32_t ImportThunk(const char* dll, const char* name);
// True and runs the handler when `target` is an import thunk.
bool DispatchImport(Ctx* ctx, uint32_t target);
void RegisterImports(const ImportDef* defs, size_t count);

// Argument i (0-based) of an import call: [esp + 4 + 4*i].
inline uint32_t Arg(Ctx* c, int i) { return R32(c->esp + 4 + 4 * i); }
inline float ArgF(Ctx* c, int i) { return RF32(c->esp + 4 + 4 * i); }
inline double ArgD(Ctx* c, int i) { return RF64(c->esp + 4 + 4 * i); }
// Return from a stdcall import (callee pops `args` dwords) or a cdecl one (args = 0).
inline void Return(Ctx* c, uint32_t eax, int args = 0) {
    c->eax = eax;
    c->ret_target = Pop32(c);
    c->esp += 4 * args;
}
// x87 return value (st0) for CRT math functions.
inline void ReturnF(Ctx* c, double v) {
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
    c->ret_target = Pop32(c);
}

// Each shim module registers its table from a static initializer.
struct ImportRegistrar {
    template <size_t N>
    explicit ImportRegistrar(const ImportDef (&defs)[N]) { RegisterImports(defs, N); }
};

// ---- PE loading (loader.cpp) ----
struct LoadedImage {
    uint32_t base, size, entry;
};
// Maps a PE32 image at `load_base` (0: its preferred base; otherwise base relocations are applied), resolving
// imports to thunks. Returns false when the file is unusable.
bool LoadImage(const char* path, LoadedImage* out, uint32_t load_base = 0);
// Where the native host loads the engine. Not its preferred 0x10000000: on Android ART's heap lives there, and its
// size varies (main space 512 MiB or 1 GiB from 0x02000000, then 512 MiB of large object space, reaching
// 0x62000000), so the engine sits above ART's boot image and non-moving space (which end near 0x77000000). The
// recompiled code must be generated for the same address (recomp.py --base 0x7c000000); below 2 GiB because the
// game is not large-address aware.
constexpr uint32_t kEngineBase = 0x7c000000u;
constexpr uint32_t kEngineReserve = 0x02000000u;  // address space kept out of the arena for the image
uint32_t FindExport(const LoadedImage& image, const char* name);

// Address range of the engine image (thunk following in the dispatcher).
extern uint32_t g_engine_lo, g_engine_hi;

// Directory with the game files (cwd for the engine's relative paths).
extern const char* g_game_dir;
extern const char* g_data_dir;  // Android: the app's external files folder; nullptr elsewhere

// ---- settings (config.cpp): protore.ini, overridden by PROTORE_<KEY> environment variables ----
std::string Setting(const char* key, const char* def);
int SettingInt(const char* key, int def);

// Android performance hints (perf_hint.cpp): frame budget and per-frame work for the system's CPU clocking.
void PerfHintStart(int fps_target);
void PerfHintAddThread(int tid);
void PerfHintTarget(int fps_target);
void PerfHintFrame(int64_t work_ns);

}  // namespace native
