// Differential tests closing out CementLibrary: platform wrappers, Free/FreeObject, IoBufferHolder, the small
// library methods, open-by-key, and construction/destruction. Parts that detour functions used by other engine
// threads run under WorldStop and record into fixed-size logs.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../core/cement_library.h"
#include "../core/file_system.h"
#include "../core/memory.h"
#include "../core/mutex.h"
#include "../host/hooks.h"
#include "../host/world_stop.h"
#include "tests.h"

namespace tests {

namespace {

// ---- shared fixed-size event log ----------------------------------------------------------------------------

struct Event {
    uint32_t kind, a, b, c, d;
};

struct EventLog {
    static constexpr int kMax = 48;
    Event events[kMax];
    int count = 0;
    bool overflow = false;
    void Add(uint32_t kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
        if (count == kMax) {
            overflow = true;
            return;
        }
        events[count++] = {kind, a, b, c, d};
    }
    bool operator==(const EventLog& o) const {
        return count == o.count && overflow == o.overflow && memcmp(events, o.events, sizeof(Event) * count) == 0;
    }
};

EventLog g_log;

uint32_t U(const void* p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

uint32_t Fingerprint(const char* s) {
    uint32_t h = 0;
    for (; s && *s; ++s) h = h * 131 + static_cast<uint8_t>(*s);
    return h;
}

template <typename Fn>
Fn Original(uint32_t rva) {
    return reinterpret_cast<Fn>(protore::OriginalFunction(rva));
}

template <typename T>
T& Global(uint32_t rva) {
    return *static_cast<T*>(protore::EngineAddress(rva));
}

// Generic recording object: every vtable slot records (id, slot) and returns id ^ slot.
struct Recorder {
    void** vtable;
    uint32_t id;
};

template <int Slot>
uint32_t __thiscall RecordSlot(Recorder* self) {
    g_log.Add('O', self->id, Slot);
    return self->id ^ Slot;
}

template <int... Slots>
struct RecorderVtable {
    void* slots[40];
    RecorderVtable() {
        for (void*& s : slots) s = reinterpret_cast<void*>(&RecordSlot<-1>);
        ((slots[Slots] = reinterpret_cast<void*>(&RecordSlot<Slots>)), ...);
    }
};

RecorderVtable<0, 1, 2, 17, 21> g_object_vtable;

// Stream queue (slot 18) takes two arguments; record them too.
void __thiscall RecordQueue(Recorder* self, void* callback, int32_t tag) {
    g_log.Add('Q', self->id, U(callback), static_cast<uint32_t>(tag));
}

// ---- detour recorders ---------------------------------------------------------------------------------------

constexpr uint8_t kFreeBytes[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x0d, 0xa4, 0xc5, 0x0b, 0x11};
void __cdecl RecordFree(uint32_t heap, void* p) {
    g_log.Add('F', heap, U(p));
}
void __cdecl RecordFreeObject(uint32_t heap, void* p) {
    g_log.Add('G', heap, U(p));
}

constexpr uint8_t kVReportBytes[] = {0x8b, 0x4c, 0x24, 0x14, 0x8b, 0x44, 0x24, 0x18};
int __cdecl RecordVReport(int kind, const char* tag, const char* file, int line, const char* expr, const char*,
                          va_list) {
    g_log.Add('V', static_cast<uint32_t>(kind) << 16 | static_cast<uint32_t>(line), Fingerprint(tag),
              Fingerprint(file), Fingerprint(expr));
    return 0;
}

// ---- part 1: mutex and sleep wrappers -----------------------------------------------------------------------

void TestPlatformWrappers(protore::DiffResult* r) {
    using MutexFn = void(__cdecl*)(core::Mutex*);
    using SleepFn = void(__cdecl*)(uint32_t);
    CRITICAL_SECTION a, b;
    auto* ma = reinterpret_cast<core::Mutex*>(&a);
    auto* mb = reinterpret_cast<core::Mutex*>(&b);
    Original<MutexFn>(0x0063d260)(ma);
    core::MutexInit(mb);
    for (int depth = 1; depth <= 3; ++depth) {
        Original<MutexFn>(0x0063d290)(ma);
        core::MutexLock(mb);
        protore::Check(r, a.RecursionCount == b.RecursionCount && a.LockCount == b.LockCount,
                       "lock depth %d: recursion %ld/%ld lock %ld/%ld", depth, a.RecursionCount, b.RecursionCount,
                       a.LockCount, b.LockCount);
    }
    for (int depth = 3; depth >= 1; --depth) {
        Original<MutexFn>(0x0063d280)(ma);
        core::MutexUnlock(mb);
        protore::Check(r, a.RecursionCount == b.RecursionCount && a.OwningThread == b.OwningThread,
                       "unlock to depth %d differs", depth - 1);
    }
    Original<MutexFn>(0x0063d270)(ma);
    core::MutexDestroy(mb);
    // Sleep(0) only yields; both must return.
    Original<SleepFn>(0x00644b40)(0);
    core::ThreadSleep(0);
}

// ---- part 2: Free / FreeObject --------------------------------------------------------------------------------

struct FakeAllocator {
    void** vtable;
    uint32_t id;
};
void __thiscall AllocatorFreeObject(FakeAllocator* a, void* p) {
    g_log.Add('4', a->id, U(p));
}
void __thiscall AllocatorFree(FakeAllocator* a, void* p) {
    g_log.Add('6', a->id, U(p));
}

void TestFree(protore::DiffResult* r) {
    using FreeFn = void(__cdecl*)(uint32_t, void*);
    static void* vtable[8];
    for (void*& s : vtable) s = nullptr;
    vtable[4] = reinterpret_cast<void*>(&AllocatorFreeObject);
    vtable[6] = reinterpret_cast<void*>(&AllocatorFree);
    static FakeAllocator allocators[2] = {{vtable, 0xa0}, {vtable, 0xa1}};
    static core::HeapRecord table[2] = {{&allocators[0], {}}, {&allocators[1], {}}};
    EventLog a, b;
    {
        protore::WorldStop stop;
        auto& table_ptr = Global<core::HeapRecord*>(0x010bc5a4);
        core::HeapRecord* saved = table_ptr;
        table_ptr = table;
        for (uint32_t heap : {0u, 1u}) {
            for (uintptr_t p : {uintptr_t(0), uintptr_t(0x1234560)}) {
                g_log = EventLog();
                Original<FreeFn>(0x0063a9c0)(heap, reinterpret_cast<void*>(p));
                Original<FreeFn>(0x0063a9a0)(heap, reinterpret_cast<void*>(p));
                a = g_log;
                g_log = EventLog();
                core::Free(heap, reinterpret_cast<void*>(p));
                core::FreeObject(heap, reinterpret_cast<void*>(p));
                b = g_log;
                if (!(a == b)) break;
            }
        }
        table_ptr = saved;
    }
    protore::Check(r, a == b && a.count == 2, "Free/FreeObject allocator calls differ");
}

// ---- part 3: IoBufferHolder -----------------------------------------------------------------------------------

struct FakeLibrary {
    void** vtable;
    uint32_t id;
    uint8_t pad[0x68 - 8];
    core::CementLibrary* next;  // +0x68
    uint32_t list;              // +0x6c
};
static_assert(offsetof(FakeLibrary, next) == 0x68 && offsetof(FakeLibrary, list) == 0x6c, "fake library layout");

constexpr int kLibraries = 6;

struct HolderWorld {
    core::IoBufferHolder holder;  // lock initialised once, never copied
    FakeLibrary libraries[kLibraries];
};

HolderWorld g_holder_world;

void BuildHolderWorld(HolderWorld* w) {
    core::IoBufferHolder& h = w->holder;
    memset(&h, 0, offsetof(core::IoBufferHolder, lock));
    h.buffer = reinterpret_cast<void*>(0xbbbb0000);
    h.size = 0x8000;
    h.references = 0;
    h.heap = 7;
    for (int i = 0; i < kLibraries; ++i) {
        FakeLibrary& l = w->libraries[i];
        memset(&l, 0x5a, sizeof(l));
        l.vtable = g_object_vtable.slots;
        l.id = 0x100 + i;
        l.next = reinterpret_cast<core::CementLibrary*>(0xdeadbeef);
        l.list = (i == 2 || i == 4) ? 1 : 0;
    }
}

core::CementLibrary* Lib(HolderWorld* w, int i) {
    return reinterpret_cast<core::CementLibrary*>(&w->libraries[i]);
}

// Holder state minus the lock bytes, plus every library's link field.
struct HolderSnapshot {
    uint8_t before_lock[0x14];
    uint8_t after_lock[0x10];
    core::CementLibrary* next[kLibraries];
    bool operator==(const HolderSnapshot& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

HolderSnapshot Snapshot(const HolderWorld& w) {
    HolderSnapshot s;
    memcpy(s.before_lock, &w.holder, 0x14);
    memcpy(s.after_lock, reinterpret_cast<const uint8_t*>(&w.holder) + 0x2c, 0x10);
    for (int i = 0; i < kLibraries; ++i) s.next[i] = w.libraries[i].next;
    return s;
}

struct HolderApi {
    void(__thiscall* reg)(core::IoBufferHolder*, core::CementLibrary*);
    void(__thiscall* unreg)(core::IoBufferHolder*, core::CementLibrary*);
    void(__thiscall* begin)(core::IoBufferHolder*, uint32_t);
    core::CementLibrary*(__thiscall* next)(core::IoBufferHolder*);
    void(__thiscall* end)(core::IoBufferHolder*);
    void(__thiscall* release_buffer)(core::IoBufferHolder*);
};

// One scripted scenario; returns the values Next() produced (encoded) into the log.
void RunHolderScript(const HolderApi& api, HolderWorld* w, int script) {
    core::IoBufferHolder* h = &w->holder;
    switch (script) {
        case 0:  // register all, unregister head, middle, tail, absent, then iterate both lists
            for (int i = 0; i < kLibraries; ++i) api.reg(h, Lib(w, i));
            api.unreg(h, Lib(w, 0));
            api.unreg(h, Lib(w, 3));
            api.unreg(h, Lib(w, 5));
            api.unreg(h, Lib(w, 5));
            for (uint32_t list : {0u, 1u}) {
                api.begin(h, list);
                while (core::CementLibrary* l = api.next(h)) g_log.Add('N', U(l) - U(w));
                api.end(h);
            }
            break;
        case 1:  // unregister the cursor element mid-iteration
            for (int i = 0; i < kLibraries; ++i) api.reg(h, Lib(w, i));
            api.begin(h, 0);
            g_log.Add('N', U(api.next(h)) - U(w));
            api.unreg(h, Lib(w, 1));  // cursor currently points at library 1
            while (core::CementLibrary* l = api.next(h)) g_log.Add('N', U(l) - U(w));
            api.end(h);
            break;
        case 2:  // empty list operations
            api.unreg(h, Lib(w, 0));
            api.begin(h, 1);
            g_log.Add('N', U(api.next(h)));
            api.end(h);
            break;
        case 3:  // drain one list completely, then re-register
            for (int i : {2, 4}) api.reg(h, Lib(w, i));
            api.unreg(h, Lib(w, 4));
            api.unreg(h, Lib(w, 2));
            api.reg(h, Lib(w, 4));
            break;
        case 4:  // buffer references 2 -> 1 -> 0 (the last frees)
            h->references = 2;
            api.release_buffer(h);
            api.release_buffer(h);
            break;
        default:  // a stray release below zero
            h->references = 0;
            api.release_buffer(h);
            break;
    }
}

void TestHolder(protore::DiffResult* r) {
    const HolderApi original = {
        Original<decltype(HolderApi::reg)>(0x00650580), Original<decltype(HolderApi::unreg)>(0x006505d0),
        Original<decltype(HolderApi::begin)>(0x00650640), Original<decltype(HolderApi::next)>(0x00650660),
        Original<decltype(HolderApi::end)>(0x00650670), Original<decltype(HolderApi::release_buffer)>(0x006506f0)};
    const HolderApi reconstructed = {&core::IoBufferHolderRegister,       &core::IoBufferHolderUnregister,
                                     &core::IoBufferHolderBeginIteration, &core::IoBufferHolderNext,
                                     &core::IoBufferHolderEndIteration,   &core::IoBufferHolderReleaseBuffer};
    HolderWorld* w = &g_holder_world;
    core::MutexInit(reinterpret_cast<core::Mutex*>(w->holder.lock));

    constexpr int kScripts = 6;
    bool state_ok[kScripts], events_ok[kScripts], lock_ok[kScripts];
    {
        protore::WorldStop stop;
        protore::TestDetour free(0x0063a9c0, sizeof(kFreeBytes), kFreeBytes, sizeof(kFreeBytes),
                                 reinterpret_cast<void*>(&RecordFree));
        for (int script = 0; script < kScripts; ++script) {
            BuildHolderWorld(w);
            g_log = EventLog();
            RunHolderScript(original, w, script);
            HolderSnapshot snap_a = Snapshot(*w);
            EventLog log_a = g_log;
            auto* cs = reinterpret_cast<CRITICAL_SECTION*>(w->holder.lock);
            LONG recursion_a = cs->RecursionCount;

            BuildHolderWorld(w);
            g_log = EventLog();
            RunHolderScript(reconstructed, w, script);
            state_ok[script] = Snapshot(*w) == snap_a;
            events_ok[script] = g_log == log_a;
            lock_ok[script] = cs->RecursionCount == recursion_a && recursion_a == 0;
        }
    }
    for (int script = 0; script < kScripts; ++script) {
        protore::Check(r, state_ok[script], "holder script %d: holder/link state differs", script);
        protore::Check(r, events_ok[script], "holder script %d: iteration/free events differ", script);
        protore::Check(r, lock_ok[script], "holder script %d: lock left held", script);
    }
    core::MutexDestroy(reinterpret_cast<core::Mutex*>(w->holder.lock));

    // Init and ReleaseLibraries create/destroy the lock: run each implementation on its own holder.
    using InitFn = core::IoBufferHolder*(__thiscall*)(core::IoBufferHolder*);
    using ReleaseAllFn = void(__thiscall*)(core::IoBufferHolder*);
    static HolderWorld wa, wb;
    BuildHolderWorld(&wa);
    BuildHolderWorld(&wb);
    memset(&wa.holder, 0x77, sizeof(wa.holder));
    memset(&wb.holder, 0x77, sizeof(wb.holder));
    core::IoBufferHolder* ra = Original<InitFn>(0x00650b60)(&wa.holder);
    core::IoBufferHolder* rb = core::IoBufferHolderInit(&wb.holder);
    protore::Check(r, ra == &wa.holder && rb == &wb.holder, "Init return value");
    protore::Check(r, Snapshot(wa) == Snapshot(wb), "Init: fields differ");
    for (int i = 0; i < kLibraries; ++i) {
        reconstructed.reg(&wa.holder, Lib(&wa, i));
        reconstructed.reg(&wb.holder, Lib(&wb, i));
    }
    g_log = EventLog();
    Original<ReleaseAllFn>(0x00650540)(&wa.holder);
    EventLog log_a = g_log;
    g_log = EventLog();
    core::IoBufferHolderReleaseLibraries(&wb.holder);
    // Both worlds use the same library ids, so the (id, slot) sequences are directly comparable.
    protore::Check(r, log_a == g_log && log_a.count == kLibraries, "ReleaseLibraries: release calls differ (%d vs %d)",
                   log_a.count, g_log.count);
}

// ---- part 4: small library methods ---------------------------------------------------------------------------

constexpr uint8_t kPumpBytes[] = {0x57, 0xe8, 0xda, 0xff, 0xff, 0xff};
uint8_t* g_pump_state = nullptr;
int g_pump_calls_until_loaded = 0;

void __cdecl RecordPump() {
    g_log.Add('P');
    if (--g_pump_calls_until_loaded == 0) *reinterpret_cast<int32_t*>(g_pump_state) = 1;
}

struct LockObject {
    void** vtable;
};
uint32_t __thiscall LockEnter(LockObject*) {
    g_log.Add('L', 0);
    return 0x1000;
}
uint32_t __thiscall LockLeave(LockObject*) {
    g_log.Add('L', 1);
    return 0x1001;
}
uint32_t __thiscall LockDelete(LockObject*, uint32_t flags) {
    g_log.Add('L', 2, flags);
    return 0x1002;
}
void* g_lock_vtable[4] = {reinterpret_cast<void*>(&LockEnter), reinterpret_cast<void*>(&LockLeave),
                          reinterpret_cast<void*>(&LockDelete), nullptr};

// Interface reached by the forwarders: slot n records ecx and one stack argument.
uint32_t __thiscall ForwardTarget0(void* self, uint32_t arg) {
    g_log.Add('W', 0, U(self), arg);
    return 0x2000;
}
uint32_t __thiscall ForwardTarget1(void* self, uint32_t arg) {
    g_log.Add('W', 1, U(self), arg);
    return 0x2001;
}
void* g_forward_vtable[2] = {reinterpret_cast<void*>(&ForwardTarget0), reinterpret_cast<void*>(&ForwardTarget1)};

void TestSmallMethods(protore::DiffResult* r) {
    alignas(16) static uint8_t lib_a[0x90], lib_b[0x90];
    auto* la = reinterpret_cast<core::CementLibrary*>(lib_a);
    auto* lb = reinterpret_cast<core::CementLibrary*>(lib_b);
    auto reset = [&](int32_t state, int32_t refs) {
        for (uint8_t* lib : {lib_a, lib_b}) {
            memset(lib, 0x5a, 0x90);
            memcpy(lib + 0x24, &state, 4);
            void* lock_vtable = g_lock_vtable;
            memcpy(lib + 0x10, &lock_vtable, 4);
            memcpy(lib + 0x14, &refs, 4);
        }
    };
    auto same_objects = [&] { return memcmp(lib_a, lib_b, 0x90) == 0; };

    // IsLoaded
    using IsLoadedFn = uint32_t(__thiscall*)(core::CementLibrary*);
    for (int32_t state : {0, 1, 2, -1}) {
        reset(state, 1);
        uint32_t a = Original<IsLoadedFn>(0x00650390)(la);
        uint32_t b = core::CementLibraryIsLoaded(lb);
        protore::Check(r, a == b, "IsLoaded state %d: %u != %u", state, a, b);
    }

    // SetCompletion
    using SetCompletionFn = void(__thiscall*)(core::CementLibrary*, void*, uint32_t);
    static Recorder completion{g_object_vtable.slots, 0xc0};
    reset(0, 1);
    g_log = EventLog();
    Original<SetCompletionFn>(0x006503a0)(la, &completion, 0x1234);
    EventLog log_a = g_log;
    g_log = EventLog();
    core::CementLibrarySetCompletion(lb, &completion, 0x1234);
    protore::Check(r, log_a == g_log && same_objects(), "SetCompletion differs");

    // AddRef / Release, including the release that reaches zero
    using RefFn = uint32_t(__thiscall*)(core::CementLibrary*);
    for (int32_t refs : {1, 2, 5}) {
        for (bool add : {true, false}) {
            reset(1, refs);
            g_log = EventLog();
            uint32_t a = add ? Original<RefFn>(0x00650830)(la) : Original<RefFn>(0x00650850)(la);
            log_a = g_log;
            g_log = EventLog();
            uint32_t b = add ? core::CementLibraryAddRef(lb) : core::CementLibraryRelease(lb);
            protore::Check(r, a == b && log_a == g_log && same_objects(), "%s with count %d differs",
                           add ? "AddRef" : "Release", refs);
        }
    }

    // Forwarders: slot target gets the original ecx-derived interface and the caller's stack argument.
    using ForwardFn = uint32_t(__thiscall*)(void*, uint32_t);
    struct Target {
        uint32_t pad;
        void** iface_vtable;  // +4: the interface the forwarders call into
    };
    static Target target{0, g_forward_vtable};
    reset(0, 1);
    void* target_ptr = &target;
    memcpy(lib_a + 0x10, &target_ptr, 4);
    for (int slot = 0; slot < 2; ++slot) {
        g_log = EventLog();
        uint32_t a = Original<ForwardFn>(slot ? 0x00650340 : 0x00650330)(lib_a, 0xabcd0000 + slot);
        log_a = g_log;
        g_log = EventLog();
        auto reconstructed = reinterpret_cast<ForwardFn>(slot ? &core::CementLibraryForwardSlot1
                                                              : &core::CementLibraryForwardSlot0);
        uint32_t b = reconstructed(lib_a, 0xabcd0000 + slot);
        protore::Check(r, a == b && log_a == g_log, "ForwardSlot%d differs", slot);
    }

    // WaitUntilSettled: already settled, and settling after three polls.
    using WaitFn = void(__thiscall*)(core::CementLibrary*);
    for (int polls : {0, 3}) {
        EventLog logs[2];
        for (int impl = 0; impl < 2; ++impl) {
            protore::TestDetour pump(0x0064a030, sizeof(kPumpBytes), kPumpBytes, sizeof(kPumpBytes),
                                     reinterpret_cast<void*>(&RecordPump), 1);
            reset(polls ? 0 : 1, 1);
            g_pump_state = (impl ? lib_b : lib_a) + 0x24;
            g_pump_calls_until_loaded = polls;
            g_log = EventLog();
            if (impl) {
                core::CementLibraryWaitUntilSettled(lb);
            } else {
                Original<WaitFn>(0x00650360)(la);
            }
            logs[impl] = g_log;
        }
        protore::Check(r, logs[0] == logs[1] && logs[0].count == polls, "WaitUntilSettled after %d polls differs",
                       polls);
    }
}

// ---- part 5: open by key --------------------------------------------------------------------------------------

constexpr uint8_t kNewFileBytes[] = {0x56, 0x68, 0x60, 0xca, 0x27, 0x11};
constexpr uint8_t kFileCtorBytes[] = {0x8b, 0x44, 0x24, 0x04, 0x8a, 0x54, 0x24, 0x0c};
constexpr uint8_t kNewRequestBytes[] = {0x56, 0x68, 0x44, 0xca, 0x27, 0x11};
constexpr uint8_t kRequestCtorBytes[] = {0x8b, 0x44, 0x24, 0x04, 0x56};
constexpr uint8_t kQueueBytes[] = {0x8b, 0x89, 0xd0, 0x00, 0x00, 0x00};

alignas(16) uint8_t g_file_memory[0x174];
alignas(16) uint8_t g_request_memory[0x14];
bool g_new_file_fails = false;
bool g_new_request_fails = false;

void* __cdecl RecordNewFile(uint32_t size) {
    g_log.Add('n', size);
    return g_new_file_fails ? nullptr : g_file_memory;
}
void* __cdecl RecordNewRequest(uint32_t size) {
    g_log.Add('m', size);
    return g_new_request_fails ? nullptr : g_request_memory;
}
void* __thiscall RecordFileCtor(void* self, void* drive, const char* name, uint32_t z1, uint32_t z2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6, uint32_t z3, uint32_t z4) {
    g_log.Add('f', U(self), U(drive), Fingerprint(name), z1 | z2 << 8 | z3 << 16 | z4 << 24);
    g_log.Add('g', a3, a4, a5, a6);
    return self;
}
void* __thiscall RecordRequestCtor(void* self, void* file, uint32_t key, void* library) {
    g_log.Add('r', U(self), U(file), key, U(library));
    return self;
}
void __thiscall RecordQueueRequest(void* drive, void* request, uint32_t a3, uint32_t zero) {
    g_log.Add('q', U(drive), U(request), a3, zero);
}

using OpenFn = void(__thiscall*)(core::CementLibrary*, core::File**, uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t);

// AndWait goes through the library's vtable slot 7; the fake slot records and returns a scripted file.
Recorder g_opened_file{g_object_vtable.slots, 0xf11e};
bool g_open_returns_file = true;
void __thiscall RecordSlot7(core::CementLibrary* self, core::File** out, uint32_t key, uint32_t a3, uint32_t a4,
                            uint32_t a5, uint32_t a6) {
    g_log.Add('7', U(self), key, a3 ^ a4 ^ a5 ^ a6);
    *out = g_open_returns_file ? reinterpret_cast<core::File*>(&g_opened_file) : nullptr;
}

void TestOpenByKey(protore::DiffResult* r) {
    alignas(16) static uint8_t lib[0x90];
    memset(lib, 0x5a, sizeof(lib));
    void* drive = reinterpret_cast<void*>(0xd51e0000);
    memcpy(lib + 0x18, &drive, 4);
    auto* library = reinterpret_cast<core::CementLibrary*>(lib);

    bool open_ok = true, out_ok = true;
    {
        protore::WorldStop stop;
        protore::TestDetour d1(0x0064e050, sizeof(kNewFileBytes), kNewFileBytes, sizeof(kNewFileBytes),
                               reinterpret_cast<void*>(&RecordNewFile));
        protore::TestDetour d2(0x0064e9e0, sizeof(kFileCtorBytes), kFileCtorBytes, sizeof(kFileCtorBytes),
                               reinterpret_cast<void*>(&RecordFileCtor));
        protore::TestDetour d3(0x0064c4e0, sizeof(kNewRequestBytes), kNewRequestBytes, sizeof(kNewRequestBytes),
                               reinterpret_cast<void*>(&RecordNewRequest));
        protore::TestDetour d4(0x0064d810, 5, kRequestCtorBytes, sizeof(kRequestCtorBytes),
                               reinterpret_cast<void*>(&RecordRequestCtor));
        protore::TestDetour d5(0x0064feb0, sizeof(kQueueBytes), kQueueBytes, sizeof(kQueueBytes),
                               reinterpret_cast<void*>(&RecordQueueRequest));
        open_ok = d1.ok() && d2.ok() && d3.ok() && d4.ok() && d5.ok();
        for (bool file_fails : {false, true}) {
            for (bool request_fails : {false, true}) {
                for (uint32_t key : {0u, 0x0f96ba20u, 0xffffffffu}) {
                    g_new_file_fails = file_fails;
                    g_new_request_fails = request_fails;
                    core::File* out_a = reinterpret_cast<core::File*>(1);
                    core::File* out_b = reinterpret_cast<core::File*>(2);
                    g_log = EventLog();
                    Original<OpenFn>(0x00650460)(library, &out_a, key, 11, 22, 33, 44);
                    EventLog log_a = g_log;
                    g_log = EventLog();
                    core::CementLibraryOpenByKey(library, &out_b, key, 11, 22, 33, 44);
                    if (!(log_a == g_log)) open_ok = false;
                    if (out_a != out_b) out_ok = false;
                }
            }
        }
    }
    protore::Check(r, open_ok, "OpenByKey: allocation/construction/queue calls differ");
    protore::Check(r, out_ok, "OpenByKey: *out differs");

    // AndWait: fake vtable whose slot 7 is the recorder.
    static void* vtable[32];
    for (void*& s : vtable) s = nullptr;
    vtable[7] = reinterpret_cast<void*>(&RecordSlot7);
    void* vtable_ptr = vtable;
    memcpy(lib, &vtable_ptr, 4);
    for (bool returns_file : {true, false}) {
        g_open_returns_file = returns_file;
        core::File* out_a = nullptr;
        core::File* out_b = nullptr;
        g_log = EventLog();
        Original<OpenFn>(0x00650500)(library, &out_a, 0x77, 1, 2, 3, 4);
        EventLog log_a = g_log;
        g_log = EventLog();
        core::CementLibraryOpenByKeyAndWait(library, &out_b, 0x77, 1, 2, 3, 4);
        protore::Check(r, log_a == g_log && out_a == out_b, "OpenByKeyAndWait (file %d) differs", returns_file);
    }
}

// ---- part 6: construction and destruction --------------------------------------------------------------------

struct FakeDrive {
    void** vtable;
    uint32_t id;
    uint8_t pad[0x14 - 8];
    core::IoBufferHolder holder;  // +0x14, lock initialised once
};
static_assert(offsetof(FakeDrive, holder) == 0x14, "fake drive layout");

struct LifetimeWorld {
    alignas(16) uint8_t library[0x90];
    FakeDrive drive;
    FakeDrive registration;
    Recorder stream;
    uint8_t header[0x3c];
};

LifetimeWorld g_lifetime;
void* g_stream_vtable[40];

void BuildLifetime(LifetimeWorld* w) {
    memset(w->library, 0x5a, sizeof(w->library));
    for (FakeDrive* d : {&w->drive, &w->registration}) {
        d->vtable = g_object_vtable.slots;
        memset(d->pad, 0x5a, sizeof(d->pad));
        memset(&d->holder, 0, offsetof(core::IoBufferHolder, lock));
        d->holder.buffer = reinterpret_cast<void*>(0xbbbb0000);
        d->holder.size = 0x8000;
        d->holder.references = 2;
        d->holder.heap = 7;
    }
    w->drive.id = 0xd0;
    w->registration.id = 0xd1;
    w->stream = {g_stream_vtable, 0x5e};
    for (int i = 0; i < 0x3c; ++i) w->header[i] = static_cast<uint8_t>(i * 7);
}

struct LifetimeSnapshot {
    uint8_t library[0x90];
    uint8_t holders[2][0x14 + 0x10];
    int32_t current_heap;
    uint8_t assert_flag;
    bool operator==(const LifetimeSnapshot& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

LifetimeSnapshot SnapshotLifetime(const LifetimeWorld& w) {
    LifetimeSnapshot s;
    memset(&s, 0, sizeof(s));  // compared with memcmp: padding after assert_flag must be deterministic
    memcpy(s.library, w.library, sizeof(s.library));
    const FakeDrive* drives[2] = {&w.drive, &w.registration};
    for (int i = 0; i < 2; ++i) {
        memcpy(s.holders[i], &drives[i]->holder, 0x14);
        memcpy(s.holders[i] + 0x14, reinterpret_cast<const uint8_t*>(&drives[i]->holder) + 0x2c, 0x10);
    }
    s.current_heap = Global<int32_t>(0x010bccbc);
    s.assert_flag = Global<uint8_t>(0x010bd87a);
    return s;
}

template <typename T>
void Put(uint8_t* base, uint32_t offset, T value) {
    memcpy(base + offset, &value, sizeof(value));
}

struct DestroyCase {
    int32_t state;
    bool needs_buffer, drives_equal, assert_enabled, has_entries, registered;
    int32_t object_heap;
};

// Puts the library into a post-load shape directly (independent of the constructor under test).
void SetUpLoadedLibrary(LifetimeWorld* w, const DestroyCase& c) {
    BuildLifetime(w);
    uint8_t* lib = w->library;
    Put<void*>(lib, 0x00, protore::EngineAddress(0x00e48000));
    Put<int32_t>(lib, 0x0c, c.object_heap);
    Put<void*>(lib, 0x18, c.drives_equal ? &w->registration : &w->drive);
    Put<void*>(lib, 0x1c, &w->registration);
    Put<void*>(lib, 0x20, &w->stream);
    Put<int32_t>(lib, 0x24, c.state);
    Put<uint32_t>(lib, 0x5c, c.needs_buffer ? 1 : 0);
    Put<void*>(lib, 0x64, c.has_entries ? reinterpret_cast<void*>(0xe0e00000) : nullptr);
    Put<uint32_t>(lib, 0x6c, 0);
    Put<uint32_t>(lib, 0x7c, 9);
    if (c.registered) {
        core::IoBufferHolderRegister(&w->registration.holder, reinterpret_cast<core::CementLibrary*>(lib));
    }
}

struct Difference {
    bool recorded = false;
    char text[192];
};

void Describe(Difference* d, const char* what, const LifetimeSnapshot& a, const LifetimeSnapshot& b,
              const EventLog& la, const EventLog& lb) {
    if (d->recorded) return;
    d->recorded = true;
    const auto* pa = reinterpret_cast<const uint8_t*>(&a);
    const auto* pb = reinterpret_cast<const uint8_t*>(&b);
    int byte = -1;
    for (size_t i = 0; i < sizeof(LifetimeSnapshot); ++i) {
        if (pa[i] != pb[i]) {
            byte = static_cast<int>(i);
            break;
        }
    }
    int event = -1;
    for (int i = 0; i < la.count || i < lb.count; ++i) {
        if (i >= la.count || i >= lb.count || memcmp(&la.events[i], &lb.events[i], sizeof(Event)) != 0) {
            event = i;
            break;
        }
    }
    const Event ea = event >= 0 && event < la.count ? la.events[event] : Event{};
    const Event eb = event >= 0 && event < lb.count ? lb.events[event] : Event{};
    // Can't call vsnprintf-heavy Log while the world is stopped; format with wsprintfA-free plain code later.
    snprintf(d->text, sizeof(d->text),
             "%s: first byte %d (%02x vs %02x), events %d/%d first diff #%d (%c %x %x %x vs %c %x %x %x)", what, byte,
             byte >= 0 ? pa[byte] : 0, byte >= 0 ? pb[byte] : 0, la.count, lb.count, event, ea.kind ? ea.kind : '-',
             ea.a, ea.b, ea.c, eb.kind ? eb.kind : '-', eb.a, eb.b, eb.c);
}

Difference g_construct_diff, g_destruct_diff, g_deleting_diff;

void TestLifetime(protore::DiffResult* r) {
    for (void*& s : g_stream_vtable) s = g_object_vtable.slots[0];
    for (int slot : {0, 1, 17}) g_stream_vtable[slot] = g_object_vtable.slots[slot];
    g_stream_vtable[18] = reinterpret_cast<void*>(&RecordQueue);
    LifetimeWorld* w = &g_lifetime;
    for (FakeDrive* d : {&w->drive, &w->registration}) core::MutexInit(reinterpret_cast<core::Mutex*>(d->holder.lock));

    using ConstructFn = core::CementLibrary*(__thiscall*)(core::CementLibrary*, void*, void*, uint32_t,
                                                          const core::CementPreloaded*, void*);
    using DestructFn = void(__thiscall*)(core::CementLibrary*);
    using DeletingFn = core::CementLibrary*(__thiscall*)(core::CementLibrary*, uint32_t);

    int construct_cases = 0, construct_bad = 0, destruct_cases = 0, destruct_bad = 0, deleting_bad = 0;
    bool detours_ok = false;
    {
        protore::WorldStop stop;
        protore::TestDetour free(0x0063a9c0, sizeof(kFreeBytes), kFreeBytes, sizeof(kFreeBytes),
                                 reinterpret_cast<void*>(&RecordFree));
        protore::TestDetour free_object(0x0063a9a0, sizeof(kFreeBytes), kFreeBytes, sizeof(kFreeBytes),
                                        reinterpret_cast<void*>(&RecordFreeObject));
        protore::TestDetour vreport(0x0063c340, sizeof(kVReportBytes), kVReportBytes, sizeof(kVReportBytes),
                                    reinterpret_cast<void*>(&RecordVReport));
        detours_ok = free.ok() && free_object.ok() && vreport.ok();
        int32_t& current_heap = Global<int32_t>(0x010bccbc);
        uint8_t& assert_flag = Global<uint8_t>(0x010bd87a);
        const int32_t saved_heap = current_heap;
        const uint8_t saved_flag = assert_flag;

        // Constructor
        for (bool preload : {false, true}) {
            for (int32_t heap : {-1, 3}) {
                for (uint32_t list : {0u, 1u}) {
                    LifetimeSnapshot snaps[2];
                    EventLog logs[2];
                    core::CementLibrary* results[2];
                    for (int impl = 0; impl < 2; ++impl) {
                        BuildLifetime(w);
                        current_heap = heap;
                        core::CementPreloaded pre{w->header, reinterpret_cast<void*>(0xe0e00000), 4};
                        auto* lib = reinterpret_cast<core::CementLibrary*>(w->library);
                        g_log = EventLog();
                        results[impl] = (impl ? &core::CementLibraryConstruct : Original<ConstructFn>(0x00650ba0))(
                            lib, &w->drive, &w->stream, list, preload ? &pre : nullptr, &w->registration);
                        logs[impl] = g_log;
                        snaps[impl] = SnapshotLifetime(*w);
                    }
                    ++construct_cases;
                    if (!(snaps[0] == snaps[1]) || !(logs[0] == logs[1]) || results[0] != results[1]) {
                        ++construct_bad;
                        Describe(&g_construct_diff, "constructor", snaps[0], snaps[1], logs[0], logs[1]);
                    }
                }
            }
        }

        // Destructor and deleting destructor
        for (int32_t state : {0, 1, 2}) for (bool needs_buffer : {false, true}) for (bool drives_equal : {false, true})
        for (bool assert_enabled : {false, true}) for (bool has_entries : {false, true})
        for (bool registered : {false, true}) for (int32_t object_heap : {-1, 6}) {
            const DestroyCase c{state, needs_buffer, drives_equal, assert_enabled, has_entries, registered,
                                object_heap};
            LifetimeSnapshot snaps[2];
            EventLog logs[2];
            for (int impl = 0; impl < 2; ++impl) {
                SetUpLoadedLibrary(w, c);
                current_heap = -1;
                assert_flag = assert_enabled;
                g_log = EventLog();
                auto* lib = reinterpret_cast<core::CementLibrary*>(w->library);
                (impl ? &core::CementLibraryDestruct : Original<DestructFn>(0x00650730))(lib);
                logs[impl] = g_log;
                snaps[impl] = SnapshotLifetime(*w);
            }
            ++destruct_cases;
            if (!(snaps[0] == snaps[1]) || !(logs[0] == logs[1])) {
                ++destruct_bad;
                Describe(&g_destruct_diff, "destructor", snaps[0], snaps[1], logs[0], logs[1]);
            }

            for (uint32_t flags : {0u, 1u}) {
                for (int impl = 0; impl < 2; ++impl) {
                    SetUpLoadedLibrary(w, c);
                    current_heap = -1;
                    assert_flag = assert_enabled;
                    g_log = EventLog();
                    auto* lib = reinterpret_cast<core::CementLibrary*>(w->library);
                    core::CementLibrary* result =
                        (impl ? &core::CementLibraryDeletingDestruct : Original<DeletingFn>(0x00650cb0))(lib, flags);
                    g_log.Add('=', U(result) - U(w));
                    logs[impl] = g_log;
                    snaps[impl] = SnapshotLifetime(*w);
                }
                if (!(snaps[0] == snaps[1]) || !(logs[0] == logs[1])) {
                    ++deleting_bad;
                    Describe(&g_deleting_diff, "deleting destructor", snaps[0], snaps[1], logs[0], logs[1]);
                }
            }
        }
        current_heap = saved_heap;
        assert_flag = saved_flag;
    }
    for (FakeDrive* d : {&w->drive, &w->registration}) core::MutexDestroy(reinterpret_cast<core::Mutex*>(d->holder.lock));
    protore::Log("difftest CementLibrary lifetime: %d constructor and %d destructor scenarios", construct_cases,
                 destruct_cases);
    for (Difference* d : {&g_construct_diff, &g_destruct_diff, &g_deleting_diff}) {
        if (d->recorded) protore::Log("difftest lifetime detail: %s", d->text);
    }
    protore::Check(r, detours_ok, "lifetime detours unavailable");
    protore::Check(r, construct_bad == 0, "%d of %d constructor scenarios differ", construct_bad, construct_cases);
    protore::Check(r, destruct_bad == 0, "%d of %d destructor scenarios differ", destruct_bad, destruct_cases);
    protore::Check(r, deleting_bad == 0, "%d of %d deleting-destructor scenarios differ", deleting_bad,
                   destruct_cases * 2);
}

}  // namespace

void TestCementLifetime(protore::DiffResult* r) {
    TestPlatformWrappers(r);
    TestFree(r);
    TestHolder(r);
    TestSmallMethods(r);
    TestOpenByKey(r);
    TestLifetime(r);
}

}  // namespace tests
