// Guest threads and fibers. Each guest thread runs on a host thread with its own TEB and guest stack in guest
// memory. Fibers get their own guest stack and host stack; SwitchToFiber swaps host stacks with a small context
// switch (x86-64 and arm64; bionic has no swapcontext).

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "guest.h"

namespace native {

extern thread_local Ctx* tl_ctx;

namespace {

thread_local GuestThread* tl_thread = nullptr;
std::atomic<uint32_t> g_next_thread_id{0x100};

// TEB fields the engine and the MSVC CRT read through fs.
constexpr uint32_t kTebExceptionList = 0x00;
constexpr uint32_t kTebStackBase = 0x04;
constexpr uint32_t kTebStackLimit = 0x08;
constexpr uint32_t kTebFiberData = 0x10;
constexpr uint32_t kTebSelf = 0x18;
constexpr uint32_t kTebClientId = 0x20;
constexpr uint32_t kTebTlsSlots = 0xe10;
constexpr uint32_t kTebSize = 0x1000;

}  // namespace

GuestThread* AttachThread(uint32_t stack_size) {
    auto* t = new GuestThread();
    stack_size = stack_size ? (stack_size + 0xffff) & ~0xffffu : (1u << 20) * 4;
    t->stack_lo = PageAlloc(stack_size);
    t->stack_hi = t->stack_lo + stack_size;
    t->teb = PageAlloc(kTebSize);
    t->id = g_next_thread_id.fetch_add(4);
    W32(t->teb + kTebExceptionList, 0xffffffffu);
    W32(t->teb + kTebStackBase, t->stack_hi);
    W32(t->teb + kTebStackLimit, t->stack_lo);
    W32(t->teb + kTebSelf, t->teb);
    W32(t->teb + kTebClientId, 0x10);
    W32(t->teb + kTebClientId + 4, t->id);
    tl_thread = t;
    RegisterGuestThread();
    return t;
}

GuestThread* CurrentThread() {
    if (!tl_thread) Fatal("guest call on a host thread that was never attached");
    return tl_thread;
}

uint32_t TebTlsSlot(int index) { return CurrentThread()->teb + kTebTlsSlots + 4 * index; }

// ---- host context switch ----
// native_fiber_switch(&from->sp, to->sp): saves the callee-saved registers on the current stack, stores the stack
// pointer, switches to the other stack and restores its registers. A new stack is prepared so that the restore
// "returns" into native_fiber_start with the Fiber* in a callee-saved register.
extern "C" void native_fiber_switch(void** save_sp, void* to_sp);
extern "C" void native_fiber_start();
extern "C" void NativeFiberMain(void* fiber);

#if defined(__x86_64__)
asm(".text\n"
    ".globl native_fiber_switch\n"
    "native_fiber_switch:\n\t"
    "pushq %rbp\n\tpushq %rbx\n\tpushq %r12\n\tpushq %r13\n\tpushq %r14\n\tpushq %r15\n\t"
    "movq %rsp, (%rdi)\n\t"
    "movq %rsi, %rsp\n\t"
    "popq %r15\n\tpopq %r14\n\tpopq %r13\n\tpopq %r12\n\tpopq %rbx\n\tpopq %rbp\n\t"
    "ret\n"
    ".globl native_fiber_start\n"
    "native_fiber_start:\n\t"
    "movq %r12, %rdi\n\t"
    "andq $-16, %rsp\n\t"
    "call NativeFiberMain\n\t"
    "ud2\n");
constexpr int kSavedWords = 6;  // r15 r14 r13 r12 rbx rbp, then the return address
void* PrepareStack(void* top, void* fiber) {
    auto* sp = reinterpret_cast<uintptr_t*>((reinterpret_cast<uintptr_t>(top) & ~uintptr_t(15)) - 8);
    *--sp = reinterpret_cast<uintptr_t>(&native_fiber_start);
    for (int i = 0; i < kSavedWords; ++i) *--sp = 0;
    sp[3] = reinterpret_cast<uintptr_t>(fiber);  // r12
    return sp;
}
#elif defined(__aarch64__)
asm(".text\n"
    ".globl native_fiber_switch\n"
    "native_fiber_switch:\n\t"
    "sub sp, sp, #160\n\t"
    "stp x19, x20, [sp, #0]\n\tstp x21, x22, [sp, #16]\n\tstp x23, x24, [sp, #32]\n\t"
    "stp x25, x26, [sp, #48]\n\tstp x27, x28, [sp, #64]\n\tstp x29, x30, [sp, #80]\n\t"
    "stp d8, d9, [sp, #96]\n\tstp d10, d11, [sp, #112]\n\tstp d12, d13, [sp, #128]\n\tstp d14, d15, [sp, #144]\n\t"
    "mov x2, sp\n\tstr x2, [x0]\n\tmov sp, x1\n\t"
    "ldp x19, x20, [sp, #0]\n\tldp x21, x22, [sp, #16]\n\tldp x23, x24, [sp, #32]\n\t"
    "ldp x25, x26, [sp, #48]\n\tldp x27, x28, [sp, #64]\n\tldp x29, x30, [sp, #80]\n\t"
    "ldp d8, d9, [sp, #96]\n\tldp d10, d11, [sp, #112]\n\tldp d12, d13, [sp, #128]\n\tldp d14, d15, [sp, #144]\n\t"
    "add sp, sp, #160\n\t"
    "ret\n"
    ".globl native_fiber_start\n"
    "native_fiber_start:\n\t"
    "mov x0, x19\n\t"
    "bl NativeFiberMain\n\t"
    "brk #0\n");
void* PrepareStack(void* top, void* fiber) {
    auto* sp = reinterpret_cast<uintptr_t*>((reinterpret_cast<uintptr_t>(top) & ~uintptr_t(15)) - 160);
    for (int i = 0; i < 20; ++i) sp[i] = 0;
    sp[0] = reinterpret_cast<uintptr_t>(fiber);                         // x19
    sp[11] = reinterpret_cast<uintptr_t>(&native_fiber_start);          // x30
    return sp;
}
#else
#error "no fiber context switch for this architecture"
#endif

// ---- fibers ----
namespace {

struct Fiber {
    uint32_t handle;  // guest block: [0] fiber data (GetFiberData), [4] index
    uint32_t start, param;
    uint32_t stack_lo, stack_hi;
    void* sp = nullptr;  // saved host stack pointer while switched out
    void* host_stack;
    bool is_thread;  // the converted thread itself (runs on the thread's own stacks)
};

std::mutex g_fibers_mutex;
std::vector<Fiber*> g_fibers;

Fiber* FiberOf(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_fibers_mutex);
    uint32_t index = R32(handle + 4);
    return index < g_fibers.size() ? g_fibers[index] : nullptr;
}

Fiber* NewFiber(uint32_t param) {
    auto* f = new Fiber();
    f->handle = HeapAlloc(32);
    W32(f->handle, param);
    std::lock_guard<std::mutex> lock(g_fibers_mutex);
    W32(f->handle + 4, uint32_t(g_fibers.size()));
    g_fibers.push_back(f);
    f->param = param;
    return f;
}

void SetCurrentFiber(GuestThread* t, Fiber* f) {
    t->current_fiber = f ? f->handle : 0;
    W32(t->teb + kTebFiberData, f ? f->handle : 0);
    if (f && !f->is_thread) {
        W32(t->teb + kTebStackBase, f->stack_hi);
        W32(t->teb + kTebStackLimit, f->stack_lo);
    }
}

thread_local uint32_t tl_thread_stack_hi = 0;

}  // namespace

extern "C" void NativeFiberMain(void* fiber) {
    Fiber* f = static_cast<Fiber*>(fiber);
    GuestThread* t = CurrentThread();
    t->stack_hi = f->stack_hi;  // GuestCall starts from the fiber's own guest stack
    tl_ctx = nullptr;
    GuestCall(f->start, {f->param});
    Fatal("fiber %08x returned from its start routine", f->handle);
}

namespace {

}  // namespace

uint32_t FiberConvertThread(uint32_t param) {
    GuestThread* t = CurrentThread();
    Fiber* f = NewFiber(param);
    f->is_thread = true;
    f->stack_lo = t->stack_lo;
    f->stack_hi = t->stack_hi;
    tl_thread_stack_hi = t->stack_hi;
    SetCurrentFiber(t, f);
    return f->handle;
}

uint32_t FiberCreate(uint32_t stack_size, uint32_t start, uint32_t param) {
    Fiber* f = NewFiber(param);
    f->start = start;
    stack_size = stack_size ? (stack_size + 0xffff) & ~0xffffu : (1u << 20);
    f->stack_lo = PageAlloc(stack_size);
    f->stack_hi = f->stack_lo + stack_size;
    constexpr size_t kHostStack = 4u << 20;  // recompiled C frames are larger than the guest frames
    f->host_stack = malloc(kHostStack);
    f->sp = PrepareStack(static_cast<uint8_t*>(f->host_stack) + kHostStack, f);
    return f->handle;
}

void FiberSwitch(uint32_t handle) {
    GuestThread* t = CurrentThread();
    Fiber* from = t->current_fiber ? FiberOf(t->current_fiber) : nullptr;
    Fiber* to = FiberOf(handle);
    if (!from || !to) Fatal("SwitchToFiber %08x without a current fiber", handle);
    if (from == to) return;
    // Per-fiber host state that lives in thread-locals: the active Ctx and the guest stack top.
    Ctx* saved_ctx = tl_ctx;
    uint32_t saved_stack_hi = t->stack_hi;
    SetCurrentFiber(t, to);
    native_fiber_switch(&from->sp, to->sp);
    // Resumed.
    SetCurrentFiber(t, from);
    tl_ctx = saved_ctx;
    t->stack_hi = saved_stack_hi;
}

void FiberDelete(uint32_t handle) {
    Fiber* f = FiberOf(handle);
    if (!f || f->is_thread) return;
    // The stacks stay allocated: deleting the running fiber's stacks would pull them from under it.
}

void FiberConvertToThread() {
    GuestThread* t = CurrentThread();
    SetCurrentFiber(t, nullptr);
}

}  // namespace native
