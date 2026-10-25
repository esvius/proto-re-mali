// In-process bridge between the original 32-bit engine and statically recompiled code (prototype on x86/Wine).
//
// Native -> recompiled: a per-function stub `push fn; jmp RecompEnter`. RecompEnter saves the registers, switches
// to a per-thread host stack (the recompiled C code must not use the guest stack, which the guest code itself
// pushes onto), runs fn on a Ctx and returns to the guest return address with the resulting registers.
//
// Recompiled -> native: RecompCallNative swaps the already pushed guest return address for NativeReturn, switches
// to the guest stack, loads the registers and jumps to the native code; NativeReturn collects the registers (and an
// x87 return value) back into the Ctx and resumes on the host stack.
//
// Per-thread bridge state is reached through fs:[0x14] (TIB ArbitraryUserPointer), usable from assembly without
// clobbering registers. The engine runs game flow in fibers that yield from deep inside recompiled code, so the
// state (and its host stack) is really per fiber: SwitchToFiber is wrapped to swap fs:[0x14] along with the fiber.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "../host/hooks.h"
#include "bridge.h"
#include "runtime.h"

extern "C" {
extern const struct RecompTableEntry {
    uint32_t va;
    RecompFn fn;
} g_recomp_table[];
extern const uint32_t g_recomp_count;
extern const struct RecompEntryPatch {
    uint32_t va;
    uint8_t len;
    int8_t rel32_at;
    uint8_t bytes[15];
} g_recomp_entries[];
extern const uint32_t g_recomp_entry_count;
uint8_t* RecompMemoryBase = nullptr;
volatile uint32_t RecompRestrictLo = 0;
volatile uint32_t RecompRestrictHi = 0xffffffffu;
volatile uint32_t RecompTestDetours = 0;
}

namespace {

constexpr size_t kHostStackSize = 1 << 20;

// EngineService is always recompiled: its entries count the frames that tell a healthy run from a stuck one.
constexpr uint32_t kFrameProbeVa = 0x1085eb20u;

// Packed to 4: the assembly below uses fixed offsets, and fldl/fstpl do not need 8-byte alignment.
#pragma pack(push, 4)
struct Bridge {
    uint32_t host_top;      // +0  where the next host-stack frame may start (moves down while nested)
    Ctx* native_ctx;        // +4  Ctx of the native call in progress
    uint32_t host_resume;   // +8  host esp to resume after the native call
    double st0;             // +12 x87 value returned by the native call
    uint32_t has_st0;       // +20
    uint8_t* stack_base;    // +24
};
#pragma pack(pop)
static_assert(offsetof(Bridge, native_ctx) == 4 && offsetof(Bridge, host_resume) == 8 &&
                  offsetof(Bridge, st0) == 12 && offsetof(Bridge, has_st0) == 20,
              "offsets used by the assembly below");
static_assert(offsetof(Ctx, eax) == 0 && offsetof(Ctx, esp) == 16 && offsetof(Ctx, edi) == 28,
              "Ctx register offsets used by the assembly below");

Bridge* ThreadBridge() {
    Bridge* b;
    asm volatile("movl %%fs:0x14, %0" : "=r"(b));
    if (!b) {
        b = static_cast<Bridge*>(VirtualAlloc(nullptr, sizeof(Bridge), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        b->stack_base = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kHostStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        b->host_top = reinterpret_cast<uint32_t>(b->stack_base + kHostStackSize - 64);
        asm volatile("movl %0, %%fs:0x14" : : "r"(b));
    }
    return b;
}

// Exit record consumed by RecompEnter's epilogue.
#pragma pack(push, 4)
struct ExitRecord {
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi, esp, ret_target;  // +0 .. +32
    double st0;                                                     // +36
    uint32_t has_st0;                                               // +44
};
#pragma pack(pop)
static_assert(offsetof(ExitRecord, esp) == 28 && offsetof(ExitRecord, ret_target) == 32 &&
                  offsetof(ExitRecord, st0) == 36 && offsetof(ExitRecord, has_st0) == 44,
              "ExitRecord offsets used by the assembly below");

// pushad frame as laid out on the guest stack, followed by the fn pointer pushed by the stub.
struct EntryFrame {
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    RecompFn fn;
};

}  // namespace

extern "C" uint32_t RecompHostStackTop() {
    return ThreadBridge()->host_top & ~15u;
}

extern "C" ExitRecord* RecompEnterC(EntryFrame* frame) {
    // Evidence that recompiled code actually runs: log at every power of two entries.
    static volatile LONG entries = 0;
    LONG n = InterlockedIncrement(&entries);
    if ((n & (n - 1)) == 0 && n >= 1024) {
        protore::Log("recomp: %ld entries into recompiled code", n);
    }
    // Frame counter: EngineService (rva 0x0085eb20) is called once per frame by prototypef.exe.
    static RecompFn engine_service = RecompLookup(kFrameProbeVa);
    static volatile LONG frames = 0;
    if (frame->fn == engine_service) {
        LONG f = InterlockedIncrement(&frames);
        if ((f & (f - 1)) == 0) {
            protore::Log("recomp: frames %ld", f);
        }
    }
    Bridge* b = ThreadBridge();
    const uint32_t saved_top = b->host_top;
    uint32_t here;
    asm volatile("movl %%esp, %0" : "=r"(here));
    b->host_top = here - 256;  // nested entries continue below this frame

    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.eax = frame->eax;
    ctx.ecx = frame->ecx;
    ctx.edx = frame->edx;
    ctx.ebx = frame->ebx;
    ctx.ebp = frame->ebp;
    ctx.esi = frame->esi;
    ctx.edi = frame->edi;
    ctx.esp = reinterpret_cast<uint32_t>(&frame->fn) + 4;  // the guest return address
    ctx.fpu_cw = 0x027f;
    asm volatile("movl %%fs:0x18, %0" : "=r"(ctx.fs_base));  // TEB self pointer
    frame->fn(&ctx);
    // EngineInitialize's result decides whether the exe enters its main loop or calls EngineTerminate.
    static RecompFn engine_initialize = RecompLookup(0x1085efb0u);
    if (frame->fn == engine_initialize) {
        protore::Log("recomp: EngineInitialize returned %08x", ctx.eax);
    }

    static thread_local ExitRecord exit_record;
    ExitRecord& r = exit_record;
    r.eax = ctx.eax;
    r.ecx = ctx.ecx;
    r.edx = ctx.edx;
    r.ebx = ctx.ebx;
    r.ebp = ctx.ebp;
    r.esi = ctx.esi;
    r.edi = ctx.edi;
    r.esp = ctx.esp;
    r.ret_target = ctx.ret_target;
    // A value left on the emulated x87 stack is the function's float return value.
    r.has_st0 = ctx.st_top != 0;
    r.st0 = r.has_st0 ? ctx.st[ctx.st_top & 7] : 0.0;
    b->host_top = saved_top;
    return &r;
}

asm(".text\n"
    ".globl _RecompEnter\n"
    "_RecompEnter:\n\t"
    "pushal\n\t"
    "movl %esp, %ebx\n\t"
    "call _RecompHostStackTop\n\t"
    "movl %eax, %esp\n\t"
    "pushl %ebx\n\t"
    "call _RecompEnterC\n\t"
    "movl %eax, %esi\n\t"
    "cmpl $0, 44(%esi)\n\t"
    "je 1f\n\t"
    "fldl 36(%esi)\n"
    "1:\n\t"
    "movl 0(%esi), %eax\n\t"
    "movl 4(%esi), %ecx\n\t"
    "movl 8(%esi), %edx\n\t"
    "movl 12(%esi), %ebx\n\t"
    "movl 16(%esi), %ebp\n\t"
    "movl 24(%esi), %edi\n\t"
    "movl 28(%esi), %esp\n\t"
    "pushl 32(%esi)\n\t"
    "movl 20(%esi), %esi\n\t"
    "ret\n"

    // void RecompCallNativeAsm(Ctx* c, uint32_t target): cdecl.
    ".globl _RecompCallNativeAsm\n"
    "_RecompCallNativeAsm:\n\t"
    "pushl %ebp\n\t"
    "pushl %ebx\n\t"
    "pushl %esi\n\t"
    "pushl %edi\n\t"
    "movl %fs:0x14, %eax\n\t"
    "movl %esp, 8(%eax)\n\t"     // host_resume
    "movl 20(%esp), %ecx\n\t"    // c
    "movl %ecx, 4(%eax)\n\t"     // native_ctx
    "movl %esp, 0(%eax)\n\t"     // host_top: nested entries go below this frame
    "subl $256, 0(%eax)\n\t"
    "movl 24(%esp), %edx\n\t"    // target
    "movl 16(%ecx), %ebx\n\t"    // guest esp
    "movl $_RecompNativeReturn, (%ebx)\n\t"  // replace the pushed return address
    "movl %edx, -4(%ebx)\n\t"    // target, read back after esp moves to the guest stack (dead slot below it)
    "movl 0(%ecx), %eax\n\t"
    "movl 8(%ecx), %edx\n\t"
    "movl 12(%ecx), %ebx\n\t"
    "movl 20(%ecx), %ebp\n\t"
    "movl 24(%ecx), %esi\n\t"
    "movl 28(%ecx), %edi\n\t"
    "movl 16(%ecx), %esp\n\t"
    "movl 4(%ecx), %ecx\n\t"
    "jmp *-4(%esp)\n"
    ".globl _RecompNativeReturn\n"
    "_RecompNativeReturn:\n\t"
    "pushl %eax\n\t"
    "movl %fs:0x14, %eax\n\t"
    "movl 4(%eax), %eax\n\t"     // ctx
    "movl %ecx, 4(%eax)\n\t"
    "movl %edx, 8(%eax)\n\t"
    "movl %ebx, 12(%eax)\n\t"
    "movl %ebp, 20(%eax)\n\t"
    "movl %esi, 24(%eax)\n\t"
    "movl %edi, 28(%eax)\n\t"
    "popl %ecx\n\t"
    "movl %ecx, 0(%eax)\n\t"
    "movl %esp, 16(%eax)\n\t"
    "movl %fs:0x14, %ecx\n\t"
    "movl $0, 20(%ecx)\n\t"      // has_st0 = 0
    "fnstsw %ax\n\t"
    "shrl $11, %eax\n\t"
    "andl $7, %eax\n\t"
    "je 2f\n\t"
    "fstpl 12(%ecx)\n\t"
    "movl $1, 20(%ecx)\n"
    "2:\n\t"
    "movl 8(%ecx), %esp\n\t"
    "popl %edi\n\t"
    "popl %esi\n\t"
    "popl %ebx\n\t"
    "popl %ebp\n\t"
    "ret\n");

extern "C" void RecompEnter();
extern "C" void RecompCallNativeAsm(Ctx* c, uint32_t target);

namespace {

int Compare(const void* key, const void* entry) {
    uint32_t va = *static_cast<const uint32_t*>(key);
    uint32_t other = static_cast<const RecompTableEntry*>(entry)->va;
    return va < other ? -1 : va > other ? 1 : 0;
}

}  // namespace

namespace {

// PROTORE_RECOMP_RANGE=<lo>-<hi> (hex VAs, hi exclusive): only functions in the range run recompiled; the rest stay
// native. Used to bisect a misbehaving function.
uint32_t g_range_lo = 0, g_range_hi = 0xffffffffu;

void LoadRange() {
    static bool loaded = false;
    if (loaded) {
        return;
    }
    loaded = true;
    char text[64] = "";
    if (GetEnvironmentVariableA("PROTORE_RECOMP_RANGE", text, sizeof(text)) && strchr(text, '-')) {
        g_range_lo = strtoul(text, nullptr, 16);
        g_range_hi = strtoul(strchr(text, '-') + 1, nullptr, 16);
        protore::Log("recomp: range %08x-%08x", g_range_lo, g_range_hi);
    }
    RecompRestrictLo = g_range_lo;
    RecompRestrictHi = g_range_hi;
}

// Open-addressing hash of every recompiled entry, for calls through computed addresses.
constexpr uint32_t kHashBits = 18;
struct HashSlot {
    uint32_t va;
    RecompFn fn;
};
HashSlot* g_hash = nullptr;

uint32_t HashOf(uint32_t va) {
    return (va * 2654435761u) >> (32 - kHashBits);
}

void BuildHash() {
    g_hash = static_cast<HashSlot*>(
        VirtualAlloc(nullptr, sizeof(HashSlot) << kHashBits, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    for (uint32_t i = 0; i < g_recomp_count; ++i) {
        uint32_t h = HashOf(g_recomp_table[i].va);
        while (g_hash[h].va) {
            h = (h + 1) & ((1u << kHashBits) - 1);
        }
        g_hash[h] = {g_recomp_table[i].va, g_recomp_table[i].fn};
    }
}

bool InRange(uint32_t va) {
    return va == kFrameProbeVa || (va >= g_range_lo && va < g_range_hi);
}

}  // namespace

extern "C" RecompFn RecompLookup(uint32_t target) {
    if (!InRange(target)) {
        return nullptr;
    }
    if (!g_hash) {
        BuildHash();
    }
    for (uint32_t h = HashOf(target);; h = (h + 1) & ((1u << kHashBits) - 1)) {
        if (g_hash[h].va == target) {
            return g_hash[h].fn;
        }
        if (!g_hash[h].va) {
            return nullptr;
        }
    }
}

extern "C" void RecompDispatch(Ctx* ctx, uint32_t target) {
    if (!protore::TestDetourActive(target)) {
        if (RecompFn fn = RecompLookup(target)) {
            fn(ctx);
            return;
        }
    }
    Bridge* b = ThreadBridge();
    // Native calls may nest (native code calling back into recompiled code calling native again).
    Ctx* saved_ctx = b->native_ctx;
    const uint32_t saved_resume = b->host_resume;
    const uint32_t saved_top = b->host_top;
    // Diagnostics for native CRT calls that depend on FPU state (PROTORE_TRACE_FPU=1).
    static uint32_t controlfp_s = 0;
    static int trace_fpu = -1;
    if (trace_fpu < 0) {
        trace_fpu = GetEnvironmentVariableA("PROTORE_TRACE_FPU", nullptr, 0) != 0;
        controlfp_s = reinterpret_cast<uint32_t>(GetProcAddress(GetModuleHandleA("msvcr80.dll"), "_controlfp_s"));
    }
    if (trace_fpu && target == controlfp_s) {
        uint16_t x87_cw;
        uint32_t mxcsr;
        asm volatile("fnstcw %0\n\tstmxcsr %1" : "=m"(x87_cw), "=m"(mxcsr));
        protore::Log("trace: _controlfp_s(cur=%08x, new=%08x, mask=%08x) ret=%08x host cw=%04x mxcsr=%08x guest cw=%04x",
                     R32(ctx->esp + 4), R32(ctx->esp + 8), R32(ctx->esp + 12), R32(ctx->esp), x87_cw, mxcsr,
                     ctx->fpu_cw);
    }
    // The native code pops the return address slot itself (it is replaced by RecompNativeReturn below). For a
    // tail call that slot holds the real return address of the recompiled function, so keep it as ret_target.
    const uint32_t return_slot = R32(ctx->esp);
    RecompCallNativeAsm(ctx, target);
    ctx->ret_target = return_slot;
    b->native_ctx = saved_ctx;
    b->host_resume = saved_resume;
    b->host_top = saved_top;
    if (b->has_st0) {
        ctx->st_top = (ctx->st_top - 1) & 7;
        ctx->st[ctx->st_top] = b->st0;
        b->has_st0 = 0;
    }
}

extern "C" void RecompCpuid(Ctx* ctx) {
    uint32_t a = ctx->eax, b, c = ctx->ecx, d;
    asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    ctx->eax = a;
    ctx->ebx = b;
    ctx->ecx = c;
    ctx->edx = d;
}

// ---- setjmp/longjmp ------------------------------------------------------------------------------------------

namespace {

constexpr int kJmpSlots = 64;

struct JmpSlots {
    RecompJmpSlot slots[kJmpSlots];
    int next;
};

JmpSlots* ThreadJmpSlots() {
    static thread_local JmpSlots* slots = nullptr;
    if (!slots) {
        slots = static_cast<JmpSlots*>(VirtualAlloc(nullptr, sizeof(JmpSlots), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    return slots;
}

RecompJmpSlot* FindSlot(uint32_t guest_buf, bool create) {
    JmpSlots* t = ThreadJmpSlots();
    for (int i = 0; i < kJmpSlots; ++i) {
        if (t->slots[i].guest_buf == guest_buf) {
            return &t->slots[i];
        }
    }
    if (!create) {
        return nullptr;
    }
    RecompJmpSlot* s = &t->slots[t->next];
    t->next = (t->next + 1) % kJmpSlots;
    s->guest_buf = guest_buf;
    return s;
}

}  // namespace

extern "C" void RecompBridgeSave(uint8_t state[32]) {
    Bridge* b = ThreadBridge();
    uint32_t words[3] = {b->host_top, reinterpret_cast<uint32_t>(b->native_ctx), b->host_resume};
    memcpy(state, words, sizeof(words));
}

extern "C" void RecompBridgeRestore(const uint8_t state[32]) {
    Bridge* b = ThreadBridge();
    uint32_t words[3];
    memcpy(words, state, sizeof(words));
    b->host_top = words[0];
    b->native_ctx = reinterpret_cast<Ctx*>(words[1]);
    b->host_resume = words[2];
}

extern "C" jmp_buf* RecompSetjmpPrepare(Ctx* ctx, uint32_t guest_buf) {
    RecompJmpSlot* s = FindSlot(guest_buf, true);
    s->esp = ctx->esp;
    s->ebp = ctx->ebp;
    s->ebx = ctx->ebx;
    s->esi = ctx->esi;
    s->edi = ctx->edi;
    s->st_top = ctx->st_top;
    s->fpu_cw = ctx->fpu_cw;
    s->value = 0;
    RecompBridgeSave(s->bridge_state);
    return &s->host;
}

extern "C" void RecompSetjmpResume(Ctx* ctx, uint32_t guest_buf) {
    RecompJmpSlot* s = FindSlot(guest_buf, false);
    ctx->esp = s->esp;
    ctx->ebp = s->ebp;
    ctx->ebx = s->ebx;
    ctx->esi = s->esi;
    ctx->edi = s->edi;
    ctx->st_top = s->st_top;
    ctx->fpu_cw = s->fpu_cw;
    ctx->eax = s->value;
}

extern "C" void RecompLongjmp(Ctx* ctx, uint32_t guest_buf, uint32_t value) {
    RecompJmpSlot* s = FindSlot(guest_buf, false);
    if (!s) {
        RecompUnimplemented(ctx, guest_buf, "longjmp to an unknown jmp_buf");
    }
    s->value = value ? value : 1;
    RecompBridgeRestore(s->bridge_state);
    longjmp(s->host, 1);
}

extern "C" void RecompUnimplemented(Ctx* ctx, uint32_t va, const char* what) {
    protore::Log("recomp: unimplemented instruction at %08x (%s), esp=%08x", va, what, ctx->esp);
    TerminateProcess(GetCurrentProcess(), 0x7e);
}

namespace {

// Engine IAT slot of kernel32!SwitchToFiber, called through by the engine's fiber yield.
constexpr uint32_t kSwitchToFiberIat = 0x11331c58u;

using SwitchToFiberFn = VOID(WINAPI*)(LPVOID);
SwitchToFiberFn g_switch_to_fiber = nullptr;

// Each fiber keeps its own Bridge: the outgoing one stays in this frame on the outgoing fiber's stack and is put
// back when that fiber is resumed here; a fiber running for the first time starts with none (allocated on entry).
VOID WINAPI BridgeSwitchToFiber(LPVOID fiber) {
    Bridge* mine;
    asm volatile("movl %%fs:0x14, %0" : "=r"(mine));
    asm volatile("movl %0, %%fs:0x14" : : "r"(0));
    g_switch_to_fiber(fiber);
    asm volatile("movl %0, %%fs:0x14" : : "r"(mine));
}

void InstallFiberBridge() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    auto* base = static_cast<uint8_t*>(protore::EngineAddress(0));
    auto* slot = reinterpret_cast<uint32_t*>(base + (kSwitchToFiberIat - protore::kEngineImageBase));
    DWORD old_protect;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old_protect);
    g_switch_to_fiber = reinterpret_cast<SwitchToFiberFn>(*slot);
    *slot = reinterpret_cast<uint32_t>(&BridgeSwitchToFiber);
    VirtualProtect(slot, 4, old_protect, &old_protect);
    protore::Log("recomp: SwitchToFiber wrapped (per-fiber bridge state)");
}

}  // namespace

namespace protore {

int InstallAllRecompEntries() {
    // Unprotect the engine's code once, write `jmp stub` (int3-filled) at every enterable function, restore.
    auto* base = static_cast<uint8_t*>(EngineAddress(0));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint32_t code_start = nt->OptionalHeader.BaseOfCode;
    const uint32_t code_size = nt->OptionalHeader.SizeOfCode;
    DWORD old_protect;
    if (!VirtualProtect(base + code_start, code_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        Log("recomp: cannot unprotect engine code");
        return 0;
    }
    LoadRange();
    int installed = 0, mismatched = 0;
    for (uint32_t i = 0; i < g_recomp_entry_count; ++i) {
        const RecompEntryPatch& e = g_recomp_entries[i];
        if (!InRange(e.va)) {
            continue;
        }
        auto* target = reinterpret_cast<uint8_t*>(e.va - kEngineImageBase + reinterpret_cast<uintptr_t>(base));
        if (memcmp(target, e.bytes, e.len) != 0) {
            ++mismatched;  // already patched by a hand hook or different build: leave it
            continue;
        }
        void* stub = RecompEntryStub(e.va);
        if (!stub) {
            continue;
        }
        int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - reinterpret_cast<uintptr_t>(target + 5));
        target[0] = 0xE9;
        memcpy(target + 1, &rel, 4);
        for (uint32_t k = 5; k < e.len; ++k) {
            target[k] = 0xCC;
        }
        ++installed;
    }
    VirtualProtect(base + code_start, code_size, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), base + code_start, code_size);
    Log("recomp: %d function entries redirected to recompiled code (%d skipped: bytes differ)", installed, mismatched);
    return installed;
}

void* RecompEntryStub(uint32_t va) {
    RecompFn fn = RecompLookup(va);
    if (!fn) {
        return nullptr;
    }
    InstallFiberBridge();
    static uint8_t* pool = nullptr;
    static size_t used = 0;
    if (!pool) {
        pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    }
    uint8_t* stub = pool + used;
    used += 16;
    stub[0] = 0x68;  // push imm32
    uint32_t fn_value = reinterpret_cast<uint32_t>(fn);
    memcpy(stub + 1, &fn_value, 4);
    stub[5] = 0xE9;  // jmp rel32
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&RecompEnter) - reinterpret_cast<uintptr_t>(stub + 10));
    memcpy(stub + 6, &rel, 4);
    return stub;
}

}  // namespace protore
