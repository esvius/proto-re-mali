#include <cstdint>
#include <cstring>
#include <vector>

#include "../core/cement_file_range.h"
#include "../host/hooks.h"
#include "tests.h"

namespace tests {

namespace {

constexpr uint32_t kFileRangeInitRva = 0x00651130;

using InitFn = void(__thiscall*)(core::CementFileRange*, core::Drive*, uint32_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, uint32_t);

// Fake drive: a vtable whose slots record (drive id, slot) for every call made by the code under test.
struct FakeDrive {
    void** vtable;
    uint32_t id;
    core::DriveMount mount;
};

struct VirtualCall {
    uint32_t id;
    int slot;
    bool operator==(const VirtualCall& o) const { return id == o.id && slot == o.slot; }
};

std::vector<VirtualCall> g_calls;

void __thiscall SlotReference(FakeDrive* d) {
    g_calls.push_back({d->id, 0});
}
uint32_t __thiscall SlotValue(FakeDrive* d) {
    g_calls.push_back({d->id, 16});
    return d->id * 0x01010101u;
}
core::DriveMount* __thiscall SlotMount(FakeDrive* d) {
    g_calls.push_back({d->id, 32});
    return &d->mount;
}
uint32_t __thiscall SlotUnexpected(FakeDrive* d) {
    g_calls.push_back({d->id, -1});
    return 0;
}

void* g_vtable[64];

void InitVtable() {
    for (void*& slot : g_vtable) {
        slot = reinterpret_cast<void*>(&SlotUnexpected);
    }
    g_vtable[0] = reinterpret_cast<void*>(&SlotReference);
    g_vtable[16] = reinterpret_cast<void*>(&SlotValue);
    g_vtable[32] = reinterpret_cast<void*>(&SlotMount);
}

}  // namespace

void TestCementFileRange(protore::DiffResult* r) {
    InitVtable();
    auto original = reinterpret_cast<InitFn>(protore::OriginalFunction(kFileRangeInitRva));

    const uint32_t base_offsets[] = {0x10, 0x800, 0xfffffff0u, 0x12345678};
    for (int depth = 1; depth <= 4; ++depth) {
        FakeDrive drives[4];
        for (int i = 0; i < depth; ++i) {
            drives[i].vtable = g_vtable;
            drives[i].id = 0xd0 + i;
            drives[i].mount.base_offset = base_offsets[i];
            drives[i].mount.parent = i + 1 < depth ? reinterpret_cast<core::Drive*>(&drives[i + 1]) : nullptr;
        }
        auto* drive = reinterpret_cast<core::Drive*>(&drives[0]);
        for (uint32_t offset : {0u, 0x3c0u, 0xfffffff8u}) {
            for (uint32_t extra : {0u, 0x77u}) {
                core::CementFileRange a, b;
                memset(&a, 0xcd, sizeof(a));
                memset(&b, 0xcd, sizeof(b));
                g_calls.clear();
                original(&a, drive, offset, 0x1111, 0x2222, extra, 0xa0a0a0a0, 0xb0b0b0b0, 0x7a970000);
                std::vector<VirtualCall> calls_a;
                calls_a.swap(g_calls);
                core::CementFileRangeInit(&b, drive, offset, 0x1111, 0x2222, extra, 0xa0a0a0a0, 0xb0b0b0b0,
                                          0x7a970000);
                std::vector<VirtualCall> calls_b;
                calls_b.swap(g_calls);
                protore::Check(r, memcmp(&a, &b, sizeof(a)) == 0,
                               "depth %d offset %08x extra %x: range differs (offset %08x vs %08x)", depth, offset,
                               extra, a.offset, b.offset);
                protore::Check(r, calls_a == calls_b, "depth %d offset %08x: drive call sequences differ (%u vs %u)",
                               depth, offset, static_cast<unsigned>(calls_a.size()),
                               static_cast<unsigned>(calls_b.size()));
            }
        }
    }
}

}  // namespace tests
