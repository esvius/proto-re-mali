// Differential tests for core::File and the counted event: pool, construction/destruction, reference counting,
// request accounting and waiting, every asynchronous operation and synchronous wrapper, and the accessors.
// Engine functions that background threads also use are detoured only while the world is stopped, and recorders
// write into fixed-size logs.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../core/event.h"
#include "../core/file.h"
#include "../host/hooks.h"
#include "../host/world_stop.h"
#include "tests.h"

namespace tests {

namespace {

// ---- log -------------------------------------------------------------------------------------------------------

struct Event {
    uint32_t kind, a, b, c, d, e;
};

struct EventLog {
    static constexpr int kMax = 32;
    Event events[kMax];
    int count = 0;
    bool overflow = false;
    void Add(uint32_t kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, uint32_t e = 0) {
        if (count == kMax) {
            overflow = true;
            return;
        }
        events[count++] = {kind, a, b, c, d, e};
    }
    bool operator==(const EventLog& o) const {
        return count == o.count && overflow == o.overflow && memcmp(events, o.events, sizeof(Event) * count) == 0;
    }
};

EventLog g_log;

uint32_t U(const void* p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

template <typename Fn>
Fn Original(uint32_t rva) {
    return reinterpret_cast<Fn>(protore::OriginalFunction(rva));
}

template <typename T>
T& Global(uint32_t rva) {
    return *static_cast<T*>(protore::EngineAddress(rva));
}

// ---- fake objects ----------------------------------------------------------------------------------------------

// Drive: records slots 0, 1, 3, 4 (one stack argument), 8 (returns a scripted byte).
struct FakeDrive {
    void** vtable;
    uint32_t id;
    int8_t notify_result;
};

uint32_t __thiscall DriveAddRef(FakeDrive* d) { g_log.Add('d', d->id, 0); return 0x10; }
uint32_t __thiscall DriveRelease(FakeDrive* d) { g_log.Add('d', d->id, 1); return 0x11; }
uint32_t __thiscall DriveService(FakeDrive* d) { g_log.Add('d', d->id, 3); return 0x13; }
uint32_t __thiscall DriveSlot4(FakeDrive* d, uint32_t arg) { g_log.Add('d', d->id, 4, arg); return 0x14; }
int8_t __thiscall DriveNotify(FakeDrive* d) { g_log.Add('d', d->id, 8); return d->notify_result; }
uint32_t __thiscall DriveUnexpected(FakeDrive* d) { g_log.Add('d', d->id, 0xff); return 0; }

void* g_drive_vtable[40];

// Lock sub-object at File+4: slot 0 lock, slot 1 unlock (returns a marker).
uint32_t __thiscall LockEnter(void* lock) { g_log.Add('l', 0, U(lock)); return 0x20; }
uint32_t __thiscall LockLeave(void* lock) { g_log.Add('l', 1, U(lock)); return 0x21; }
void* g_lock_vtable[4] = {reinterpret_cast<void*>(&LockEnter), reinterpret_cast<void*>(&LockLeave), nullptr, nullptr};

// File's own vtable for the slots File methods call through: 2 deleting, 3..8 operations, 20 is-idle, 21 wait.
int g_idle_after = 0;
uint32_t __thiscall FileDeleting(core::File* f, uint32_t flags) { g_log.Add('f', 2, flags, U(f)); return 0x32; }
uint32_t __thiscall FileOp3(core::File*, uint32_t a, uint32_t b, uint32_t c) { g_log.Add('f', 3, a, b, c); return 0x33; }
uint32_t __thiscall FileOp4(core::File*, uint32_t a, uint32_t b, uint32_t c) { g_log.Add('f', 4, a, b, c); return 0x34; }
uint32_t __thiscall FileOp5(core::File*, uint32_t a) { g_log.Add('f', 5, a); return 0x35; }
uint32_t __thiscall FileOp6(core::File*, uint32_t a) { g_log.Add('f', 6, a); return 0x36; }
uint32_t __thiscall FileOp7(core::File*, uint32_t a) { g_log.Add('f', 7, a); return 0x37; }
uint32_t __thiscall FileOp8(core::File*) { g_log.Add('f', 8); return 0x38; }
bool __thiscall FileIdle(core::File*) { g_log.Add('f', 20); return g_idle_after-- <= 0; }
uint32_t __thiscall FileWait(core::File*) { g_log.Add('f', 21); return 0x3f; }

void* g_file_vtable[24];

void InitVtables() {
    for (void*& s : g_drive_vtable) s = reinterpret_cast<void*>(&DriveUnexpected);
    g_drive_vtable[0] = reinterpret_cast<void*>(&DriveAddRef);
    g_drive_vtable[1] = reinterpret_cast<void*>(&DriveRelease);
    g_drive_vtable[3] = reinterpret_cast<void*>(&DriveService);
    g_drive_vtable[4] = reinterpret_cast<void*>(&DriveSlot4);
    g_drive_vtable[8] = reinterpret_cast<void*>(&DriveNotify);
    for (void*& s : g_file_vtable) s = nullptr;
    g_file_vtable[2] = reinterpret_cast<void*>(&FileDeleting);
    g_file_vtable[3] = reinterpret_cast<void*>(&FileOp3);
    g_file_vtable[4] = reinterpret_cast<void*>(&FileOp4);
    g_file_vtable[5] = reinterpret_cast<void*>(&FileOp5);
    g_file_vtable[6] = reinterpret_cast<void*>(&FileOp6);
    g_file_vtable[7] = reinterpret_cast<void*>(&FileOp7);
    g_file_vtable[8] = reinterpret_cast<void*>(&FileOp8);
    g_file_vtable[20] = reinterpret_cast<void*>(&FileIdle);
    g_file_vtable[21] = reinterpret_cast<void*>(&FileWait);
}

FakeDrive g_drive{g_drive_vtable, 0xd0, 0};
FakeDrive g_other_drive{g_drive_vtable, 0xd1, 0};

core::Drive* D(FakeDrive* d) {
    return reinterpret_cast<core::Drive*>(d);
}

// A File in a known state, with fake vtables (methods under test never read the engine's own vtables).
void BuildFile(core::File* f) {
    memset(f, 0x5a, sizeof(*f));
    f->vtable = g_file_vtable;
    f->lock_vtable = g_lock_vtable;
    f->references = 1;
    f->drive = D(&g_drive);
    f->priority = 7;
    f->status = 0x55;
    f->pending = 0;
    memset(&f->event, 0, sizeof(f->event));
    f->event_created = 0;
    f->waiting = 0;
    f->open = 0;
    strcpy(f->name, "art\\test.p3d");
}

// ---- engine recorders (all exact stack shapes of the callers) ---------------------------------------------------

bool g_new_fails = false;
alignas(16) uint8_t g_request_memory[0x40];

void* __cdecl RecNewRequest(uint32_t size) { g_log.Add('n', size); return g_new_fails ? nullptr : g_request_memory; }
uint32_t __thiscall RecQueue(void* drive, void* request, uint32_t priority, uint32_t flag) {
    g_log.Add('q', U(drive), U(request), priority, flag);
    return 0x40;
}
template <int Id>
void* __thiscall RecCtor1(void* self, void* file) { g_log.Add('c', Id, U(self), U(file)); return self; }
template <int Id>
void* __thiscall RecCtor2(void* self, void* file, uint32_t a) { g_log.Add('c', Id, U(self), U(file), a); return self; }
template <int Id>
void* __thiscall RecCtor4(void* self, void* p, uint32_t a, uint32_t b, uint32_t c) {
    g_log.Add('c', Id, U(self), U(p), a, b ^ (c << 16));
    return self;
}
void* __thiscall RecCtor5(void* self, void* file, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    g_log.Add('c', 0x870, U(self), U(file), a ^ (b << 8), c ^ (d << 16));
    return self;
}
void* __thiscall RecCtorD5E0(void* self, uint32_t a, void* drive) { g_log.Add('c', 0x5e0, U(self), a, U(drive)); return self; }
void __cdecl RecFileFree(core::File* f) { g_log.Add('F', U(f)); }
void __cdecl RecEventSignal(core::CountedEvent* e, uint32_t amount) { g_log.Add('s', U(e), amount); }
void __cdecl RecEventCreate(core::CountedEvent* e, uint32_t initial) { g_log.Add('e', U(e), initial); }
void __cdecl RecEventReset(core::CountedEvent* e, uint32_t count) { g_log.Add('r', U(e), count); }
void __cdecl RecEventWait(core::CountedEvent* e) { g_log.Add('w', U(e)); }
void __cdecl RecEventDestroy(core::CountedEvent* e) { g_log.Add('x', U(e)); }
void __cdecl RecMutexDestroy(void* m) { g_log.Add('M', U(m)); }
void __cdecl RecFreeTable(void* t) { g_log.Add('T', U(t)); }
void __cdecl RecReportOpenFiles() { g_log.Add('R'); }

constexpr uint8_t kCtorBytes[] = {0x8b, 0x44, 0x24, 0x04, 0x56};  // shared by the request constructors
constexpr uint8_t kCtorD5E0Bytes[] = {0x56, 0x8b, 0xf1, 0xe8, 0xb8, 0xff, 0xff, 0xff};
constexpr uint8_t kNewRequestBytes[] = {0x56, 0x68, 0x44, 0xca, 0x27, 0x11};
constexpr uint8_t kQueueBytes[] = {0x8b, 0x89, 0xd0, 0x00, 0x00, 0x00};
constexpr uint8_t kFileFreeBytes[] = {0x56, 0x68, 0x60, 0xca, 0x27, 0x11};
constexpr uint8_t kSignalBytes[] = {0x8b, 0x54, 0x24, 0x08, 0x85, 0xd2};
constexpr uint8_t kCreateBytes[] = {0x8b, 0x44, 0x24, 0x08, 0x33, 0xc9};
constexpr uint8_t kResetBytes[] = {0x8b, 0x4c, 0x24, 0x08, 0x56};
constexpr uint8_t kWaitBytes[] = {0x8b, 0x4c, 0x24, 0x04, 0x56};
constexpr uint8_t kDestroyEventBytes[] = {0x56, 0x8b, 0x74, 0x24, 0x08};
constexpr uint8_t kMutexDestroyBytes[] = {0x8b, 0x44, 0x24, 0x04, 0x50};
constexpr uint8_t kFreeTableBytes[] = {0x56, 0x8b, 0x74, 0x24, 0x08};
constexpr uint8_t kReportOpenFilesBytes[] = {0x56, 0x68, 0x60, 0xca, 0x27, 0x11};

#define DETOUR(var, rva, bytes, fn, ...) \
    protore::TestDetour var(rva, sizeof(bytes), bytes, sizeof(bytes), reinterpret_cast<void*>(fn), ##__VA_ARGS__)

// Runs `body(impl)` for impl 0 (original) and 1 (reconstruction) on a freshly built file, comparing the logs and
// the file bytes. Returns true when both match.
template <typename Build, typename Body>
bool SameRun(core::File* f, Build build, Body body, bool compare_event = true) {
    static core::File after_original;
    EventLog log_original;
    for (int impl = 0; impl < 2; ++impl) {
        build(f);
        g_log = EventLog();
        uint32_t result = body(impl);
        g_log.Add('=', result);
        if (impl == 0) {
            log_original = g_log;
            after_original = *f;
        }
    }
    if (!compare_event) {
        memcpy(&after_original.event, &f->event, sizeof(f->event));
    }
    return log_original == g_log && memcmp(&after_original, f, sizeof(*f)) == 0;
}

core::File g_file;

}  // namespace

void TestFile(protore::DiffResult* r) {
    InitVtables();
    core::File* f = &g_file;
    auto build = [](core::File* x) { BuildFile(x); };

    // ---- counted event (no detours: real Win32 events, single-threaded and never blocking) ----
    {
        using CreateFn = void(__cdecl*)(core::CountedEvent*, uint32_t);
        using CountFn = void(__cdecl*)(core::CountedEvent*, uint32_t);
        using OneFn = void(__cdecl*)(core::CountedEvent*);
        bool ok = true;
        for (uint32_t initial : {0u, 3u}) {
            core::CountedEvent a, b;
            Original<CreateFn>(0x0063d320)(&a, initial);
            core::CountedEventCreate(&b, initial);
            ok &= a.count == b.count && (WaitForSingleObject(a.handle, 0) == WaitForSingleObject(b.handle, 0));
            for (uint32_t amount : {1u, 2u, 0u, 5u}) {
                Original<CountFn>(0x0063d3d0)(&a, amount);
                core::CountedEventSignal(&b, amount);
                ok &= a.count == b.count && (WaitForSingleObject(a.handle, 0) == WaitForSingleObject(b.handle, 0));
            }
            Original<OneFn>(0x0063d400)(&a);  // count is 0 now: returns at once
            core::CountedEventWait(&b);
            Original<CountFn>(0x0063d390)(&a, 0);
            core::CountedEventReset(&b, 0);
            ok &= a.count == b.count;
            Original<OneFn>(0x0063d350)(&a);  // signalled: destroy does not block
            core::CountedEventDestroy(&b);
        }
        protore::Check(r, ok, "counted event create/signal/wait/reset differ");
        using TlsFn = void*(__cdecl*)();
        protore::Check(r, Original<TlsFn>(0x00644dc0)() == core::ThreadLocalValue(), "ThreadLocalValue differs");
    }

    bool pool_ok = true, lifetime_ok = true, ref_ok = true, accounting_ok = true, wait_ok = true, async_ok = true,
         wrappers_ok = true, accessors_ok = true, detours_ok = true;
    char first_failure[96] = "";
    auto fail = [&](bool& flag, const char* what) {
        flag = false;
        if (!first_failure[0]) snprintf(first_failure, sizeof(first_failure), "%s", what);
    };
    {
        protore::WorldStop stop;
        DETOUR(new_request, 0x0064c4e0, kNewRequestBytes, &RecNewRequest);
        DETOUR(queue, 0x0064feb0, kQueueBytes, &RecQueue);
        DETOUR(c7b0, 0x0064d7b0, kCtorBytes, &RecCtor4<0x7b0>);
        DETOUR(c870, 0x0064d870, kCtorBytes, &RecCtor5);
        DETOUR(c8d0, 0x0064d8d0, kCtorBytes, &RecCtor1<0x8d0>);
        DETOUR(c910, 0x0064d910, kCtorBytes, &RecCtor1<0x910>);
        DETOUR(c950, 0x0064d950, kCtorBytes, &RecCtor4<0x950>);
        DETOUR(c9b0, 0x0064d9b0, kCtorBytes, &RecCtor4<0x9b0>);
        DETOUR(ca10, 0x0064da10, kCtorBytes, &RecCtor2<0xa10>);
        DETOUR(ca60, 0x0064da60, kCtorBytes, &RecCtor2<0xa60>);
        DETOUR(cab0, 0x0064dab0, kCtorBytes, &RecCtor2<0xab0>);
        DETOUR(cb00, 0x0064db00, kCtorBytes, &RecCtor1<0xb00>);
        DETOUR(c670, 0x0064d670, kCtorBytes, &RecCtor4<0x670>);
        DETOUR(c5e0, 0x0064d5e0, kCtorD5E0Bytes, &RecCtorD5E0, 3);
        DETOUR(file_free, 0x0064e0b0, kFileFreeBytes, &RecFileFree);
        DETOUR(signal, 0x0063d3d0, kSignalBytes, &RecEventSignal);
        DETOUR(create, 0x0063d320, kCreateBytes, &RecEventCreate);
        DETOUR(reset, 0x0063d390, kResetBytes, &RecEventReset);
        DETOUR(wait, 0x0063d400, kWaitBytes, &RecEventWait);
        DETOUR(destroy_event, 0x0063d350, kDestroyEventBytes, &RecEventDestroy);
        detours_ok = new_request.ok() && queue.ok() && c7b0.ok() && c870.ok() && c8d0.ok() && c910.ok() &&
                     c950.ok() && c9b0.ok() && ca10.ok() && ca60.ok() && cab0.ok() && cb00.ok() && c670.ok() &&
                     c5e0.ok() && file_free.ok() && signal.ok() && create.ok() && reset.ok() && wait.ok() &&
                     destroy_event.ok();

        // ---- construction ----
        using Ctor10 = core::File*(__thiscall*)(core::File*, core::Drive*, const char*, uint32_t, uint32_t, uint32_t,
                                                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
        using Ctor7 = core::File*(__thiscall*)(core::File*, core::Drive*, const char*, uint32_t, uint32_t, uint32_t,
                                               uint32_t, uint32_t);
        static char long_name[400];
        memset(long_name, 'n', sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = 0;
        auto garbage = [](core::File* x) { memset(x, 0x5a, sizeof(*x)); };
        for (const char* name : {"art\\alex\\alex.p3d", "", static_cast<const char*>(long_name)}) {
            for (uint32_t auto_open : {0u, 1u, 0x100u}) {
                for (bool fails : {false, true}) {
                    g_new_fails = fails;
                    if (!SameRun(f, garbage, [&](int impl) {
                            return U((impl ? &core::FileConstruct : Original<Ctor10>(0x0064e9e0))(
                                f, D(&g_drive), name, 0x1ff, 4, 9, 6, 7, 8, auto_open, 10));
                        })) fail(lifetime_ok, "FileConstruct");
                    if (!SameRun(f, garbage, [&](int impl) {
                            return U((impl ? &core::FileConstructOpening : Original<Ctor7>(0x0064e8d0))(
                                f, D(&g_drive), name, 0x101, 4, 5, 6, 9));
                        })) fail(lifetime_ok, "FileConstructOpening");
                }
            }
        }
        g_new_fails = false;

        // ---- destruction ----
        using Destruct = void(__thiscall*)(core::File*);
        using Deleting = core::File*(__thiscall*)(core::File*, uint32_t);
        for (bool event_created : {false, true}) {
            auto b = [&](core::File* x) { BuildFile(x); x->event_created = event_created; };
            if (!SameRun(f, b, [&](int impl) { (impl ? &core::FileDestruct : Original<Destruct>(0x0064e100))(f); return 0u; }))
                fail(lifetime_ok, "FileDestruct");
            for (uint32_t flags : {0u, 1u}) {
                if (!SameRun(f, b, [&](int impl) {
                        return U((impl ? &core::FileDeletingDestruct : Original<Deleting>(0x0064e8b0))(f, flags));
                    })) fail(lifetime_ok, "FileDeletingDestruct");
            }
        }

        // ---- reference counting ----
        using Method = uint32_t(__thiscall*)(core::File*);
        for (int32_t refs : {1, 2, 3}) {
            for (bool open : {false, true}) {
                for (bool fails : {false, true}) {
                    g_new_fails = fails;
                    auto b = [&](core::File* x) { BuildFile(x); x->references = refs; x->open = open; };
                    if (!SameRun(f, b, [&](int impl) { return (impl ? &core::FileAddRef : Original<Method>(0x0064e150))(f); }))
                        fail(ref_ok, "FileAddRef");
                    if (!SameRun(f, b, [&](int impl) { return (impl ? &core::FileRelease : Original<Method>(0x0064e170))(f); }))
                        fail(ref_ok, "FileRelease");
                }
            }
        }
        g_new_fails = false;

        // ---- request accounting, idle, waiting, drive change ----
        using BoolMethod = bool(__thiscall*)(core::File*);
        using ChangeFn = bool(__thiscall*)(core::File*, core::Drive*);
        for (int32_t pending : {0, 1, 2}) {
            for (bool waiting : {false, true}) {
                for (bool created : {false, true}) {
                    for (bool open : {false, true}) {
                        auto b = [&](core::File* x) {
                            BuildFile(x);
                            x->pending = pending;
                            x->waiting = waiting;
                            x->event_created = created;
                            x->open = open;
                        };
                        if (!SameRun(f, b, [&](int impl) { return (impl ? &core::FileBeginRequest : Original<Method>(0x0064e7f0))(f); }))
                            fail(accounting_ok, "FileBeginRequest");
                        if (!SameRun(f, b, [&](int impl) { return (impl ? &core::FileEndRequest : Original<Method>(0x0064e810))(f); }))
                            fail(accounting_ok, "FileEndRequest");
                        if (!SameRun(f, b, [&](int impl) {
                                return uint32_t((impl ? &core::FileIsIdle : Original<BoolMethod>(0x0064e5d0))(f) ? 1 : 0);
                            })) fail(accounting_ok, "FileIsIdle");
                        if (!SameRun(f, b, [&](int impl) {
                                (impl ? &core::FileWaitBlocking : Original<Destruct>(0x0064e640))(f);
                                return 0u;
                            })) fail(wait_ok, "FileWaitBlocking");
                        for (FakeDrive* target : {&g_drive, &g_other_drive}) {
                            if (!SameRun(f, b, [&](int impl) {
                                    return uint32_t((impl ? &core::FileChangeDrive : Original<ChangeFn>(0x0064e790))(
                                               f, D(target)) ? 1 : 0);
                                })) fail(accounting_ok, "FileChangeDrive");
                        }
                    }
                }
            }
        }
        for (int polls : {0, 2}) {
            if (!SameRun(f, build, [&](int impl) {
                    g_idle_after = polls;
                    (impl ? &core::FileWaitPolling : Original<Destruct>(0x0064e600))(f);
                    return 0u;
                })) fail(wait_ok, "FileWaitPolling");
        }

        // ---- asynchronous operations ----
        using M0 = uint32_t(__thiscall*)(core::File*);
        using M1 = uint32_t(__thiscall*)(core::File*, uint32_t);
        using M2 = uint32_t(__thiscall*)(core::File*, uint32_t, uint32_t);
        using M3 = uint32_t(__thiscall*)(core::File*, uint32_t, uint32_t, uint32_t);
        for (bool fails : {false, true}) {
            g_new_fails = fails;
            struct Op0 { uint32_t rva; M0 fn; } ops0[] = {
                {0x0064e220, &core::FileQueueD910}, {0x0064e4e0, &core::FileQueueDB00}};
            for (auto& op : ops0)
                if (!SameRun(f, build, [&](int impl) { return (impl ? op.fn : Original<M0>(op.rva))(f); }))
                    fail(async_ok, "async op (no args)");
            struct Op1 { uint32_t rva; M1 fn; } ops1[] = {
                {0x0064e300, &core::FileQueueDA10}, {0x0064e350, &core::FileQueueDA60},
                {0x0064e3a0, &core::FileQueueDAB0}, {0x0064e580, &core::FileQueueD5E0}};
            for (auto& op : ops1)
                if (!SameRun(f, build, [&](int impl) { return (impl ? op.fn : Original<M1>(op.rva))(f, 0xa1); }))
                    fail(async_ok, "async op (1 arg)");
            if (!SameRun(f, build, [&](int impl) {
                    return (impl ? &core::FileQueueD670 : Original<M2>(0x0064e520))(f, 0xb1, 0xb2);
                })) fail(async_ok, "async op D670");
            struct Op3 { uint32_t rva; M3 fn; } ops3[] = {
                {0x0064e260, &core::FileQueueD950}, {0x0064e2b0, &core::FileQueueD9B0}};
            for (auto& op : ops3)
                if (!SameRun(f, build, [&](int impl) { return (impl ? op.fn : Original<M3>(op.rva))(f, 1, 2, 3); }))
                    fail(async_ok, "async op (3 args)");
        }
        g_new_fails = false;

        // ---- synchronous wrappers ----
        struct W3 { uint32_t rva; M3 fn; } w3[] = {{0x0064e3f0, &core::FileSlot3AndWait}, {0x0064e420, &core::FileSlot4AndWait}};
        for (auto& w : w3)
            if (!SameRun(f, build, [&](int impl) { return (impl ? w.fn : Original<M3>(w.rva))(f, 11, 12, 13); }))
                fail(wrappers_ok, "AndWait (3 args)");
        struct W1 { uint32_t rva; M1 fn; } w1[] = {{0x0064e450, &core::FileSlot5AndWait},
                                                   {0x0064e470, &core::FileSlot6AndWait},
                                                   {0x0064e490, &core::FileSlot7AndWait}};
        for (auto& w : w1)
            if (!SameRun(f, build, [&](int impl) { return (impl ? w.fn : Original<M1>(w.rva))(f, 21); }))
                fail(wrappers_ok, "AndWait (1 arg)");
        if (!SameRun(f, build, [&](int impl) { return (impl ? &core::FileSlot8AndWait : Original<M0>(0x0064e4b0))(f); }))
            fail(wrappers_ok, "Slot8AndWait");

        // ---- accessors ----
        if (!SameRun(f, build, [&](int impl) { return (impl ? &core::FileStatusLocked : Original<M0>(0x0064e6b0))(f); }))
            fail(accessors_ok, "FileStatusLocked");
        using NameFn = const char*(__thiscall*)(core::File*);
        for (int8_t notify : {int8_t(0), int8_t(5), int8_t(-1)}) {
            g_drive.notify_result = notify;
            if (!SameRun(f, build, [&](int impl) {
                    const char* n = (impl ? &core::FileNameChecked : Original<NameFn>(0x0064e6f0))(f);
                    return n ? U(n) - U(f) : 0u;
                })) fail(accessors_ok, "FileNameChecked");
        }
        g_drive.notify_result = 0;
        if (!SameRun(f, build, [&](int impl) {
                auto fn = impl ? reinterpret_cast<M1>(&core::FileDriveSlot4) : Original<M1>(0x0064e720);
                return fn(f, 0x4444);
            })) fail(accessors_ok, "FileDriveSlot4");
        struct G0 { uint32_t rva; M0 fn; } getters[] = {{0x0064e4d0, &core::FileField134},
                                                        {0x0064e760, &core::FileBlockSize}};
        for (auto& g : getters)
            if (!SameRun(f, build, [&](int impl) { return (impl ? g.fn : Original<M0>(g.rva))(f); }))
                fail(accessors_ok, "getter");
        using ByteFn = uint8_t(__thiscall*)(core::File*);
        if (!SameRun(f, build, [&](int impl) { return uint32_t((impl ? &core::FileFlag16c : Original<ByteFn>(0x0064e750))(f)); }))
            fail(accessors_ok, "FileFlag16c");
        using SetFn = void(__thiscall*)(core::File*, uint32_t);
        if (!SameRun(f, build, [&](int impl) { (impl ? &core::FileSetField12c : Original<SetFn>(0x0064e770))(f, 0x12c12c); return 0u; }))
            fail(accessors_ok, "FileSetField12c");
        using PtrFn = void*(__thiscall*)(core::File*);
        if (!SameRun(f, build, [&](int impl) { return U((impl ? &core::FileSubObject : Original<PtrFn>(0x0064e780))(f)) - U(f); }))
            fail(accessors_ok, "FileSubObject");
        using SubInitFn = void*(__thiscall*)(void*);
        if (!SameRun(f, build, [&](int impl) {
                return U((impl ? &core::FileSubObjectInit : Original<SubInitFn>(0x00650dd0))(f->sub_138)) - U(f);
            })) fail(accessors_ok, "FileSubObjectInit");
    }

    // ---- pool, against a fake pool object and table (world stopped; the real fs mutex is used for locking) ----
    {
        struct FakePool {
            void** vtable;
            core::File* next_allocation;
        };
        static core::File* table[8];
        static void* pool_vtable[24];
        static FakePool pool;
        struct PoolSlots {
            static uint32_t __thiscall Release(FakePool*) { g_log.Add('p', 1); return 0; }
            static void __thiscall Free(FakePool*, core::File* p) { g_log.Add('p', 4, U(p)); }
            static void __thiscall Enumerate(FakePool*, uint32_t a, uint32_t* out) { g_log.Add('p', 11, a, *out); }
            static core::File* __thiscall Allocate(FakePool* self) { g_log.Add('p', 18); return self->next_allocation; }
            static uint32_t __thiscall IndexOf(FakePool*, core::File* p) { g_log.Add('p', 19, U(p)); return 3; }
        };
        for (void*& s : pool_vtable) s = nullptr;
        pool_vtable[1] = reinterpret_cast<void*>(&PoolSlots::Release);
        pool_vtable[4] = reinterpret_cast<void*>(&PoolSlots::Free);
        pool_vtable[11] = reinterpret_cast<void*>(&PoolSlots::Enumerate);
        pool_vtable[18] = reinterpret_cast<void*>(&PoolSlots::Allocate);
        pool_vtable[19] = reinterpret_cast<void*>(&PoolSlots::IndexOf);
        pool.vtable = pool_vtable;

        using AllocFn = core::File*(__cdecl*)(uint32_t);
        using FreeFn = void(__cdecl*)(core::File*);
        using VoidFn = void(__cdecl*)();
        EventLog logs[2];
        core::File* table_after[2][8];
        uint32_t globals_after[2][3];
        protore::WorldStop stop;
        DETOUR(report, 0x0064df50, kReportOpenFilesBytes, &RecReportOpenFiles);
        DETOUR(mutex_destroy, 0x0063d270, kMutexDestroyBytes, &RecMutexDestroy);
        DETOUR(free_table, 0x0063ac00, kFreeTableBytes, &RecFreeTable);
        detours_ok &= report.ok() && mutex_destroy.ok() && free_table.ok();
        auto& pool_global = Global<void*>(0x0127ca78);
        auto& table_global = Global<core::File**>(0x0127ca7c);
        auto& count_global = Global<uint32_t>(0x0127ca80);
        void* saved_pool = pool_global;
        core::File** saved_table = table_global;
        uint32_t saved_count = count_global;
        for (int impl = 0; impl < 2; ++impl) {
            memset(table, 0, sizeof(table));
            pool_global = &pool;
            table_global = table;
            count_global = 8;
            g_log = EventLog();
            pool.next_allocation = &g_file;
            g_log.Add('=', U((impl ? &core::FileAllocate : Original<AllocFn>(0x0064e050))(0x174)));
            pool.next_allocation = nullptr;
            g_log.Add('=', U((impl ? &core::FileAllocate : Original<AllocFn>(0x0064e050))(0x174)));
            (impl ? &core::FileFree : Original<FreeFn>(0x0064e0b0))(&g_file);
            (impl ? &core::FileSystemLock : Original<VoidFn>(0x0064e200))();
            (impl ? &core::FileSystemUnlock : Original<VoidFn>(0x0064e210))();
            (impl ? &core::FilePoolShutdown : Original<VoidFn>(0x0064e850))();
            logs[impl] = g_log;
            memcpy(table_after[impl], table, sizeof(table));
            globals_after[impl][0] = U(pool_global);
            globals_after[impl][1] = U(table_global);
            globals_after[impl][2] = count_global;
        }
        pool_global = saved_pool;
        table_global = saved_table;
        count_global = saved_count;
        if (!(logs[0] == logs[1]) || memcmp(table_after[0], table_after[1], sizeof(table_after[0])) != 0 ||
            memcmp(globals_after[0], globals_after[1], sizeof(globals_after[0])) != 0) {
            fail(pool_ok, "file pool");
        }
    }

    if (first_failure[0]) protore::Log("difftest core::File first failure: %s", first_failure);
    protore::Check(r, detours_ok, "file test detours unavailable");
    protore::Check(r, pool_ok, "pool functions differ");
    protore::Check(r, lifetime_ok, "construction/destruction differs");
    protore::Check(r, ref_ok, "reference counting differs");
    protore::Check(r, accounting_ok, "request accounting / idle / drive change differs");
    protore::Check(r, wait_ok, "waiting differs");
    protore::Check(r, async_ok, "asynchronous operations differ");
    protore::Check(r, wrappers_ok, "synchronous wrappers differ");
    protore::Check(r, accessors_ok, "accessors differ");
}

}  // namespace tests
