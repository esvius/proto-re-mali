// Host services for the recompiled code (runtime.h) in the native host: dispatch of computed calls, imports,
// setjmp/longjmp, cpuid, traps.
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "guest.h"

extern "C" {
extern const struct RecompTableEntry {
    uint32_t va;
    RecompFn fn;
} g_recomp_table[];
extern const uint32_t g_recomp_count;

volatile uint32_t RecompRestrictLo = 0;
volatile uint32_t RecompRestrictHi = 0xffffffffu;
volatile uint32_t RecompTestDetours = 0;
uint8_t* RecompMemoryBase = nullptr;
}

namespace native {

namespace {

constexpr int kHashBits = 18;
struct HashSlot {
    uint32_t va;
    RecompFn fn;
};
HashSlot* g_hash = nullptr;

uint32_t HashOf(uint32_t va) { return (va * 2654435761u) >> (32 - kHashBits); }

void BuildHash() {
    g_hash = static_cast<HashSlot*>(calloc(size_t(1) << kHashBits, sizeof(HashSlot)));
    for (uint32_t i = 0; i < g_recomp_count; ++i) {
        uint32_t h = HashOf(g_recomp_table[i].va);
        while (g_hash[h].va) h = (h + 1) & ((1u << kHashBits) - 1);
        g_hash[h] = {g_recomp_table[i].va, g_recomp_table[i].fn};
    }
}

}  // namespace

// Ctx of the guest code that most recently called out to the host on this thread/fiber; host callbacks into guest
// code continue below its stack pointer.
thread_local Ctx* tl_ctx = nullptr;
uint32_t g_engine_lo = 0, g_engine_hi = 0;

}  // namespace native

using namespace native;

extern "C" RecompFn RecompLookup(uint32_t target) {
    static std::once_flag once;
    std::call_once(once, BuildHash);
    for (uint32_t h = HashOf(target);; h = (h + 1) & ((1u << kHashBits) - 1)) {
        if (g_hash[h].va == target) return g_hash[h].fn;
        if (!g_hash[h].va) return nullptr;
    }
}

// Thunks are not recompiled (Ghidra marks them as such): `jmp dword ptr [iat]` and `jmp rel32` are followed here.
static uint32_t FollowThunks(uint32_t target) {
    for (int i = 0; i < 8; ++i) {
        if (target < g_engine_lo || target >= g_engine_hi) break;
        if (R8(target) == 0xff && R8(target + 1) == 0x25) {
            target = R32(R32(target + 2));
        } else if (R8(target) == 0xe9) {
            target = target + 5 + R32(target + 1);
        } else {
            break;
        }
    }
    return target;
}

extern "C" void RecompDispatch(Ctx* ctx, uint32_t target) {
    if (RecompFn fn = RecompLookup(target)) {
        fn(ctx);
        return;
    }
    uint32_t real = FollowThunks(target);
    if (real != target) {
        if (RecompFn fn = RecompLookup(real)) {
            fn(ctx);
            return;
        }
        target = real;
    }
    Ctx* saved = tl_ctx;
    tl_ctx = ctx;
    if (!DispatchImport(ctx, target)) {
        Fatal("call to %08x: neither recompiled nor an import (return address %08x)", target, R32(ctx->esp));
    }
    tl_ctx = saved;
}

extern "C" void RecompCpuid(Ctx* ctx) {
    // A plain SSE2 Pentium 4-class CPU: the engine only probes for SSE/SSE2 and the core count.
    switch (ctx->eax) {
        case 0:
            ctx->eax = 1;
            ctx->ebx = 0x756e6547;  // "GenuineIntel"
            ctx->edx = 0x49656e69;
            ctx->ecx = 0x6c65746e;
            break;
        case 1:
            ctx->eax = 0x00000f29;
            ctx->ebx = 0x00010800;
            ctx->ecx = 0x00000001;   // SSE3
            ctx->edx = 0x078bfbffu;  // FPU..SSE2, HTT clear
            break;
        default:
            ctx->eax = ctx->ebx = ctx->ecx = ctx->edx = 0;
    }
}

extern "C" void RecompUnimplemented(Ctx* ctx, uint32_t va, const char* what) {
    Fatal("trap at %08x (%s), esp=%08x", va, what, ctx->esp);
}

extern "C" void RecompBridgeSave(uint8_t state[32]) { memset(state, 0, 32); }
extern "C" void RecompBridgeRestore(const uint8_t[32]) {}

// ---- setjmp/longjmp (same scheme as the in-process bridge: slots keyed by the guest jmp_buf) ----
namespace {

constexpr int kJmpSlots = 64;
struct JmpSlots {
    RecompJmpSlot slots[kJmpSlots];
    int next;
};
thread_local JmpSlots* tl_jmp = nullptr;

RecompJmpSlot* FindSlot(uint32_t guest_buf, bool create) {
    if (!tl_jmp) tl_jmp = static_cast<JmpSlots*>(calloc(1, sizeof(JmpSlots)));
    for (auto& s : tl_jmp->slots) {
        if (s.guest_buf == guest_buf) return &s;
    }
    if (!create) return nullptr;
    RecompJmpSlot* s = &tl_jmp->slots[tl_jmp->next];
    tl_jmp->next = (tl_jmp->next + 1) % kJmpSlots;
    s->guest_buf = guest_buf;
    return s;
}

}  // namespace

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
    if (!s) RecompUnimplemented(ctx, guest_buf, "longjmp to an unknown jmp_buf");
    s->value = value ? value : 1;
    longjmp(s->host, 1);
}

namespace native {

uint32_t GuestCall(uint32_t va, std::initializer_list<uint32_t> args, uint32_t ecx, uint32_t* edx_out) {
    RecompFn fn = RecompLookup(va);
    if (!fn) Fatal("GuestCall: %08x is not a recompiled function", va);
    GuestThread* t = CurrentThread();
    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.esp = (tl_ctx ? tl_ctx->esp - 256 : t->stack_hi - 64) & ~15u;
    ctx.fs_base = t->teb;
    ctx.fpu_cw = 0x027f;
    ctx.ecx = ecx;
    const uint32_t* a = args.end();
    while (a != args.begin()) Push32(&ctx, *--a);
    Push32(&ctx, 0xfffffff0u);  // return address; never jumped to
    Ctx* saved = tl_ctx;
    tl_ctx = nullptr;
    fn(&ctx);
    tl_ctx = saved;
    if (edx_out) *edx_out = ctx.edx;
    return ctx.eax;
}

}  // namespace native
