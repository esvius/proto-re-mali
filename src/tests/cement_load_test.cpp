#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "../core/cement_library.h"
#include "../core/file_system.h"
#include "../host/hooks.h"
#include "../host/world_stop.h"
#include "tests.h"

namespace tests {

namespace {

constexpr uint32_t kLoadStepRva = 0x006508e0;
constexpr uint32_t kAllocRva = 0x0063a8d0;
constexpr uint8_t kAllocBytes[] = {0x53, 0x55, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x1c};
constexpr uint32_t kRegisterRva = 0x00650580;  // reached through the 0x0064fef0 adjustor thunk
constexpr uint8_t kRegisterBytes[] = {0x56, 0x8b, 0xf1, 0x57, 0x8d, 0x7e, 0x14};
constexpr uint32_t kVReportRva = 0x0063c340;
constexpr uint8_t kVReportBytes[] = {0x8b, 0x4c, 0x24, 0x14, 0x8b, 0x44, 0x24, 0x18};
constexpr uint32_t kAssertFlagRva = 0x010bd89d;

using LoadStepFn = void(__thiscall*)(void*, int32_t);

// ---- event log shared by every recorder (fixed size: runs while other threads are suspended) ----
struct Event {
    uint32_t kind;  // 'S' stream slot, 'A' alloc, 'R' register, 'V' vreport
    uint32_t a, b, c, d;
};

struct EventLog {
    static constexpr int kMax = 24;
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

EventLog g_events;

uint32_t U(const void* p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

// ---- fake stream ----
struct FakeStream {
    void** vtable;
    bool is_open;
    int32_t error;
    uint32_t alignment;
};

void __thiscall StreamRead(FakeStream*, void* buffer, uint32_t size, int32_t flag) {
    g_events.Add('S', 3, U(buffer), size, static_cast<uint32_t>(flag));
}
void __thiscall StreamSeek(FakeStream*, uint32_t position) {
    g_events.Add('S', 5, position);
}
uint32_t __thiscall StreamAlignment(FakeStream* s) {
    g_events.Add('S', 16);
    return s->alignment;
}
void __thiscall StreamQueue(FakeStream*, void* callback, int32_t tag) {
    g_events.Add('S', 18, U(callback), static_cast<uint32_t>(tag));
}
int32_t __thiscall StreamError(FakeStream* s) {
    g_events.Add('S', 23);
    return s->error;
}
bool __thiscall StreamIsOpen(FakeStream* s) {
    g_events.Add('S', 24);
    return s->is_open;
}
void __thiscall StreamSetLoading(FakeStream*, int32_t loading) {
    g_events.Add('S', 31, static_cast<uint32_t>(loading));
}
uint32_t __thiscall StreamUnexpected(FakeStream*) {
    g_events.Add('S', 0xffff);
    return 0;
}

void* g_stream_vtable[40];

// ---- recorders for engine functions ----
uint8_t g_fake_entries[64];

void* __cdecl RecordAlloc(uint32_t heap, uint32_t size, uint32_t alignment) {
    g_events.Add('A', heap, size, alignment);
    return g_fake_entries;
}

void __thiscall RecordRegister(void* holder, void* library) {
    g_events.Add('R', U(holder), U(library));
}

int __cdecl RecordVReport(int kind, const char* tag, const char* file, int line, const char* expr, const char*,
                          va_list) {
    // Strings compared by content via a cheap fingerprint (no allocation).
    auto fingerprint = [](const char* s) {
        uint32_t h = 0;
        for (; s && *s; ++s) h = h * 131 + static_cast<uint8_t>(*s);
        return h;
    };
    g_events.Add('V', static_cast<uint32_t>(kind) << 16 | static_cast<uint32_t>(line), fingerprint(tag),
                 fingerprint(file), fingerprint(expr));
    return 0;
}

// ---- the library and its registration drive, in one block so a snapshot covers both ----
struct World {
    alignas(16) uint8_t library[0x90];
    alignas(16) uint8_t registration_drive[0x60];  // IoBufferHolder at +0x14
    alignas(16) uint8_t other_drive[0x60];
    FakeStream stream;
};

constexpr char kMagic[] = "ATG CORE CEMENT LIBRARY";

template <typename T>
void Put(uint8_t* base, uint32_t offset, T value) {
    memcpy(base + offset, &value, sizeof(value));
}

struct Case {
    int32_t state;
    int32_t phase;
    bool is_open;
    int32_t error;
    bool header_valid;
    bool revalidate;
    bool needs_buffer;
    bool drives_equal;
    bool assert_enabled;
    int32_t holder_references;
};

void Build(World* w, const Case& c) {
    memset(w, 0x5a, sizeof(*w));
    w->stream = {g_stream_vtable, c.is_open, c.error, 0x800};
    uint8_t* lib = w->library;
    Put<uint32_t>(lib, 0x0c, 3);                                     // I/O buffer heap
    Put<void*>(lib, 0x18, c.drives_equal ? w->registration_drive : w->other_drive);  // m_pDrive
    Put<void*>(lib, 0x1c, w->registration_drive);                    // m_pRegistrationDrive
    Put<void*>(lib, 0x20, &w->stream);
    Put<int32_t>(lib, 0x24, c.state);
    memcpy(lib + 0x28, kMagic, sizeof(kMagic));
    lib[0x48] = 2;
    lib[0x49] = 1;
    lib[0x4a] = c.header_valid ? 0 : 1;
    lib[0x4b] = 1;
    Put<uint32_t>(lib, 0x4c, 0x3c);    // directory offset
    Put<uint32_t>(lib, 0x50, 0x180);   // directory size
    Put<uint32_t>(lib, 0x5c, c.needs_buffer ? 1 : 0);
    Put<uint32_t>(lib, 0x60, 32);      // entry count
    Put<void*>(lib, 0x70, nullptr);    // no pending completion
    lib[0x78] = c.revalidate ? 1 : 0;
    Put<uint32_t>(lib, 0x7c, 5);       // directory heap
    auto* holder = reinterpret_cast<core::IoBufferHolder*>(w->registration_drive + 0x14);
    holder->references = c.holder_references;
}

World g_world;
World g_snapshot;
World g_after_original;

}  // namespace

void TestCementLoad(protore::DiffResult* r) {
    for (void*& slot : g_stream_vtable) slot = reinterpret_cast<void*>(&StreamUnexpected);
    g_stream_vtable[3] = reinterpret_cast<void*>(&StreamRead);
    g_stream_vtable[5] = reinterpret_cast<void*>(&StreamSeek);
    g_stream_vtable[16] = reinterpret_cast<void*>(&StreamAlignment);
    g_stream_vtable[18] = reinterpret_cast<void*>(&StreamQueue);
    g_stream_vtable[23] = reinterpret_cast<void*>(&StreamError);
    g_stream_vtable[24] = reinterpret_cast<void*>(&StreamIsOpen);
    g_stream_vtable[31] = reinterpret_cast<void*>(&StreamSetLoading);

    auto original = reinterpret_cast<LoadStepFn>(protore::OriginalFunction(kLoadStepRva));
    auto* assert_flag = static_cast<volatile uint8_t*>(protore::EngineAddress(kAssertFlagRva));

    int cases = 0, state_mismatch = 0, event_mismatch = 0, first_state_bad = -1, first_event_bad = -1;
    bool detours_ok = false;
    {
        protore::WorldStop stop;
        protore::TestDetour alloc(kAllocRva, sizeof(kAllocBytes), kAllocBytes, sizeof(kAllocBytes),
                                  reinterpret_cast<void*>(&RecordAlloc));
        protore::TestDetour reg(kRegisterRva, sizeof(kRegisterBytes), kRegisterBytes, sizeof(kRegisterBytes),
                                reinterpret_cast<void*>(&RecordRegister));
        protore::TestDetour vreport(kVReportRva, sizeof(kVReportBytes), kVReportBytes, sizeof(kVReportBytes),
                                    reinterpret_cast<void*>(&RecordVReport));
        detours_ok = alloc.ok() && reg.ok() && vreport.ok();
        const uint8_t saved_flag = *assert_flag;
        for (int32_t state : {0, 1}) for (int32_t phase : {0, 1, 2, 3}) for (bool is_open : {false, true})
        for (int32_t error : {0, 5}) for (bool header_valid : {false, true}) for (bool revalidate : {false, true})
        for (bool needs_buffer : {false, true}) for (bool drives_equal : {false, true})
        for (bool assert_enabled : {false, true}) for (int32_t refs : {0, 1}) {
            if (!detours_ok) break;
            const Case c{state, phase, is_open, error, header_valid, revalidate, needs_buffer, drives_equal,
                         assert_enabled, refs};
            Build(&g_world, c);
            g_snapshot = g_world;

            *assert_flag = assert_enabled;
            g_events = EventLog();
            original(g_world.library + 4, phase);
            EventLog events_original = g_events;
            const uint8_t flag_original = *assert_flag;
            g_after_original = g_world;

            g_world = g_snapshot;
            *assert_flag = assert_enabled;
            g_events = EventLog();
            core::CementLibraryLoadStep(g_world.library + 4, phase);

            ++cases;
            if (memcmp(&g_world, &g_after_original, sizeof(World)) != 0 || flag_original != *assert_flag) {
                if (first_state_bad < 0) first_state_bad = cases - 1;
                ++state_mismatch;
            }
            if (!(events_original == g_events)) {
                if (first_event_bad < 0) first_event_bad = cases - 1;
                ++event_mismatch;
            }
        }
        *assert_flag = saved_flag;
    }
    protore::Check(r, detours_ok, "test detours unavailable");
    protore::Check(r, cases > 0, "no cases ran");
    protore::Log("difftest core::CementLibraryLoadStep: %d scenarios compared", cases);
    protore::Check(r, state_mismatch == 0, "%d of %d cases leave different object/flag state (first #%d)",
                   state_mismatch, cases, first_state_bad);
    protore::Check(r, event_mismatch == 0, "%d of %d cases make different calls (first #%d)", event_mismatch, cases,
                   first_event_bad);
}

}  // namespace tests
