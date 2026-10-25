#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "../core/file_system.h"
#include "../core/memory.h"
#include "../host/hooks.h"
#include "../host/world_stop.h"
#include "tests.h"

namespace tests {

namespace {

constexpr uint32_t kAllocRva = 0x0063a8d0;
constexpr uint32_t kRoundUpRva = 0x00645300;
constexpr uint32_t kIoBufferSizeFnRva = 0x0064a130;
constexpr uint32_t kAcquireRva = 0x006506b0;
constexpr uint32_t kHeapTableRva = 0x010bc5a4;
constexpr uint32_t kOomHandlerRva = 0x01277f8c;
constexpr uint32_t kOomContextRva = 0x01277f90;
// push ebx; push ebp; push esi; push edi; mov edi,[esp+1Ch]
constexpr uint8_t kAllocBytes[] = {0x53, 0x55, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x1c};

using AllocFn = void*(__cdecl*)(uint32_t, uint32_t, uint32_t);
using RoundUpFn = uint32_t(__cdecl*)(uint32_t, uint32_t);
using IoBufferSizeFn = uint32_t(__cdecl*)();
using AcquireFn = void(__thiscall*)(core::IoBufferHolder*, uint32_t, uint32_t, uint32_t);

// Fixed-size call logs: nothing may allocate while other threads are suspended.
struct CallLog {
    static constexpr int kMax = 16;
    uint32_t words[kMax][6];
    int count = 0;
    void Add(uint32_t a, uint32_t b, uint32_t c, uint32_t d = 0, uint32_t e = 0, uint32_t f = 0) {
        if (count < kMax) {
            uint32_t* w = words[count++];
            w[0] = a, w[1] = b, w[2] = c, w[3] = d, w[4] = e, w[5] = f;
        }
    }
    bool operator==(const CallLog& o) const {
        return count == o.count && memcmp(words, o.words, sizeof(words[0]) * count) == 0;
    }
};

CallLog g_log;

struct FakeAllocator {
    void** vtable;
    uint32_t id;
    void* result;
};

void* __thiscall FakeAllocate(FakeAllocator* a, uint32_t size, uint32_t alignment) {
    g_log.Add(1, a->id, size, alignment);
    return a->result;
}

void __cdecl FakeOomHandler(uint32_t context, uint32_t heap, uint32_t size, uint32_t alignment, void* result) {
    g_log.Add(2, context, heap, size, alignment, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(result)));
}

void* __cdecl RecordAlloc(uint32_t heap, uint32_t size, uint32_t alignment) {
    g_log.Add(3, heap, size, alignment);
    return reinterpret_cast<void*>(0xb0f00000 + heap);
}

template <typename T>
T& Global(uint32_t rva) {
    return *static_cast<T*>(protore::EngineAddress(rva));
}

}  // namespace

void TestMemory(protore::DiffResult* r) {
    // RoundUp and FileSystemIoBufferSize are pure.
    auto original_round = reinterpret_cast<RoundUpFn>(protore::OriginalFunction(kRoundUpRva));
    for (uint32_t value : {0u, 1u, 3u, 4u, 511u, 512u, 513u, 0x7fffffffu, 0xfffffff0u, 0xffffffffu}) {
        for (uint32_t granularity : {0u, 1u, 3u, 4u, 512u, 0x8000u, 0x80000000u, 0xffffffffu}) {
            uint32_t a = original_round(value, granularity);
            uint32_t b = core::RoundUp(value, granularity);
            protore::Check(r, a == b, "RoundUp(%08x, %08x): %08x != %08x", value, granularity, a, b);
        }
    }
    auto original_size = reinterpret_cast<IoBufferSizeFn>(protore::OriginalFunction(kIoBufferSizeFnRva));
    protore::Check(r, original_size() == core::FileSystemIoBufferSize(), "FileSystemIoBufferSize differs");

    // Alloc against a fake heap table and OOM handler, with every other thread suspended.
    auto original_alloc = reinterpret_cast<AllocFn>(protore::OriginalFunction(kAllocRva));
    static void* vtable[8];
    for (void*& slot : vtable) slot = nullptr;
    vtable[5] = reinterpret_cast<void*>(&FakeAllocate);
    static FakeAllocator allocators[3] = {
        {vtable, 0xa0, reinterpret_cast<void*>(0x10000)},
        {vtable, 0xa1, nullptr},  // fails: exercises the OOM handler
        {vtable, 0xa2, reinterpret_cast<void*>(0x30000)},
    };
    static core::HeapRecord table[3] = {{&allocators[0], {}}, {&allocators[1], {}}, {&allocators[2], {}}};

    struct Case { uint32_t heap, size, alignment; bool handler; };
    const Case cases[] = {
        {0, 64, 0, true}, {0, 64, 3, true}, {0, 64, 4, true}, {2, 0, 16, true},
        {1, 128, 8, true}, {1, 128, 8, false}, {1, 0xffffffff, 1, true},
    };
    constexpr int kCases = sizeof(cases) / sizeof(cases[0]);
    CallLog logs_a[kCases], logs_b[kCases];
    void* results_a[kCases];
    void* results_b[kCases];
    {
        protore::WorldStop stop;
        auto& table_ptr = Global<core::HeapRecord*>(kHeapTableRva);
        auto& handler = Global<void*>(kOomHandlerRva);
        auto& context = Global<uint32_t>(kOomContextRva);
        core::HeapRecord* saved_table = table_ptr;
        void* saved_handler = handler;
        uint32_t saved_context = context;
        table_ptr = table;
        context = 0xc0c0c0c0;
        for (int i = 0; i < kCases; ++i) {
            handler = cases[i].handler ? reinterpret_cast<void*>(&FakeOomHandler) : nullptr;
            g_log = CallLog();
            results_a[i] = original_alloc(cases[i].heap, cases[i].size, cases[i].alignment);
            logs_a[i] = g_log;
            g_log = CallLog();
            results_b[i] = core::Alloc(cases[i].heap, cases[i].size, cases[i].alignment);
            logs_b[i] = g_log;
        }
        table_ptr = saved_table;
        handler = saved_handler;
        context = saved_context;
    }
    for (int i = 0; i < kCases; ++i) {
        protore::Check(r, results_a[i] == results_b[i], "Alloc case %d: %p != %p", i, results_a[i], results_b[i]);
        protore::Check(r, logs_a[i] == logs_b[i], "Alloc case %d: allocator/handler calls differ", i);
    }

    // IoBufferHolderAcquire with Alloc replaced by a recorder (world stopped: every thread allocates).
    auto original_acquire = reinterpret_cast<AcquireFn>(protore::OriginalFunction(kAcquireRva));
    struct AcquireCase { int32_t references; uint32_t heap, alignment, granularity; };
    const AcquireCase acquire_cases[] = {
        {0, 0, 16, 0}, {0, 3, 4, 512}, {0, 1, 0x1000, 0x8000}, {2, 3, 4, 512}, {-1, 0, 16, 7},
    };
    constexpr int kAcquireCases = sizeof(acquire_cases) / sizeof(acquire_cases[0]);
    core::IoBufferHolder holders_a[kAcquireCases], holders_b[kAcquireCases];
    CallLog acquire_logs_a[kAcquireCases], acquire_logs_b[kAcquireCases];
    {
        protore::WorldStop stop;
        protore::TestDetour detour(kAllocRva, sizeof(kAllocBytes), kAllocBytes, sizeof(kAllocBytes),
                                   reinterpret_cast<void*>(&RecordAlloc));
        for (int i = 0; i < kAcquireCases; ++i) {
            const AcquireCase& c = acquire_cases[i];
            memset(&holders_a[i], 0x5a, sizeof(holders_a[i]));
            holders_a[i].references = c.references;
            holders_b[i] = holders_a[i];
            g_log = CallLog();
            original_acquire(&holders_a[i], c.heap, c.alignment, c.granularity);
            acquire_logs_a[i] = g_log;
            g_log = CallLog();
            core::IoBufferHolderAcquire(&holders_b[i], c.heap, c.alignment, c.granularity);
            acquire_logs_b[i] = g_log;
        }
        protore::Check(r, detour.ok(), "Alloc detour unavailable");
    }
    for (int i = 0; i < kAcquireCases; ++i) {
        protore::Check(r, memcmp(&holders_a[i], &holders_b[i], sizeof(holders_a[i])) == 0,
                       "Acquire case %d: holder differs", i);
        protore::Check(r, acquire_logs_a[i] == acquire_logs_b[i], "Acquire case %d: Alloc calls differ", i);
    }
}

}  // namespace tests
