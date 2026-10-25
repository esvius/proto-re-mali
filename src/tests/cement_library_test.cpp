#include <cstdint>
#include <cstring>
#include <vector>

#include "../core/cement_library.h"
#include "../core/name_hash.h"
#include "../host/hooks.h"
#include "tests.h"

namespace tests {

namespace {

constexpr uint32_t kFindEntryRva = 0x006502c0;
constexpr uint32_t kFindAndOpenRva = 0x00650ab0;
constexpr uint32_t kOpenByNameRva = 0x00650cf0;
constexpr uint32_t kOpenRangeRva = 0x00651130;
// push ebx; mov ebx,[esp+8]
constexpr uint8_t kOpenRangeBytes[] = {0x53, 0x8b, 0x5c, 0x24, 0x08};

using FindEntryFn = const void*(__cdecl*)(const void*, uint32_t, uint32_t, uint32_t);
using FindAndOpenFn = bool(__thiscall*)(core::CementLibrary*, uint32_t, core::CementOpenRequest*);
using OpenByNameFn = bool(__thiscall*)(core::CementLibrary*, const char*, core::CementOpenRequest*);

// ---- CementFindEntry ----------------------------------------------------------------------------------------

std::vector<uint32_t> MakeTable(uint32_t count, uint32_t stride_words, uint32_t seed, bool with_duplicates) {
    std::vector<uint32_t> table(count * stride_words);
    uint32_t key = seed;
    for (uint32_t i = 0; i < count; ++i) {
        if (!(with_duplicates && i % 3 == 1)) {
            key += 1 + (seed + i * 2654435761u) % 1000;  // strictly increasing unless duplicating
        }
        table[i * stride_words] = key;
        for (uint32_t w = 1; w < stride_words; ++w) {
            table[i * stride_words + w] = i * 16 + w;  // payload identifies the record
        }
    }
    return table;
}

void TestFindEntry(protore::DiffResult* r) {
    auto original = reinterpret_cast<FindEntryFn>(protore::OriginalFunction(kFindEntryRva));
    for (uint32_t stride_words : {3u, 5u}) {
        for (uint32_t count : {0u, 1u, 2u, 3u, 7u, 8u, 64u, 1001u}) {
            for (bool duplicates : {false, true}) {
                std::vector<uint32_t> table = MakeTable(count, stride_words, 0x1234 + count, duplicates);
                std::vector<uint32_t> queries = {0u, 0xffffffffu};
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t k = table[i * stride_words];
                    queries.push_back(k);
                    queries.push_back(k - 1);
                    queries.push_back(k + 1);
                }
                for (uint32_t q : queries) {
                    const void* a = original(table.data(), count, q, stride_words * 4);
                    const void* b = core::CementFindEntry(table.data(), count, q, stride_words * 4);
                    protore::Check(r, a == b, "FindEntry stride %u count %u dup %d key %08x: %p != %p",
                                   stride_words * 4, count, duplicates, q, a, b);
                }
            }
        }
    }
}

// ---- FindAndOpen / OpenByName -----------------------------------------------------------------------------

struct OpenRangeCall {
    void* request;
    uint32_t args[8];
    bool operator==(const OpenRangeCall& o) const {
        return request == o.request && memcmp(args, o.args, sizeof(args)) == 0;
    }
};

std::vector<OpenRangeCall> g_open_range_calls;

void __thiscall RecordOpenRange(void* request, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                uint32_t a6, uint32_t a7, uint32_t a8) {
    g_open_range_calls.push_back({request, {a1, a2, a3, a4, a5, a6, a7, a8}});
}

class OpenRangeCapture {
public:
    OpenRangeCapture()
        : detour_(kOpenRangeRva, sizeof(kOpenRangeBytes), kOpenRangeBytes, sizeof(kOpenRangeBytes),
                  reinterpret_cast<void*>(&RecordOpenRange)) {
        g_open_range_calls.clear();
    }
    bool ok() const { return detour_.ok(); }
    std::vector<OpenRangeCall> Take() {
        std::vector<OpenRangeCall> out;
        out.swap(g_open_range_calls);
        return out;
    }

private:
    protore::TestDetour detour_;
};

// Minimal stand-in for the engine object: only the fields the lookup reads are meaningful.
struct FakeLibrary {
    alignas(4) uint8_t self[0x80] = {};
    alignas(4) uint8_t owner[0x60] = {};
    std::vector<uint32_t> entries;

    template <typename T>
    void Set(uint8_t* base, uint32_t offset, T value) {
        memcpy(base + offset, &value, sizeof(value));
    }

    FakeLibrary(int32_t state, bool compact, const std::vector<const char*>& names) {
        uint32_t stride_words = compact ? 3 : 5;
        // Sorted by key as in a real directory.
        std::vector<uint32_t> keys;
        for (const char* n : names) {
            keys.push_back(core::HashFileName(n));
        }
        for (size_t i = 1; i < keys.size(); ++i) {
            for (size_t j = i; j > 0 && keys[j - 1] > keys[j]; --j) {
                std::swap(keys[j - 1], keys[j]);
            }
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            entries.push_back(keys[i]);
            for (uint32_t w = 1; w < stride_words; ++w) {
                entries.push_back(0x1000 * (i + 1) + w);
            }
        }
        Set<void*>(self, 0x18, owner);
        Set<uint32_t>(self, 0x20, 0xd51e0000);
        Set<int32_t>(self, 0x24, state);
        Set<uint8_t>(self, 0x49, compact ? 1 : 0);
        Set<uint32_t>(self, 0x5c, 0x7a970000);
        Set<uint32_t>(self, 0x60, static_cast<uint32_t>(keys.size()));
        Set<const void*>(self, 0x64, entries.data());
        Set<uint32_t>(owner, 0x14 + 0x2c, 0xa0a0a0a0);
        Set<uint32_t>(owner, 0x14 + 0x30, 0xb0b0b0b0);
    }

    core::CementLibrary* library() { return reinterpret_cast<core::CementLibrary*>(self); }
};

const char* const kDirectoryNames[] = {
    "art\\alex\\alex.p3d.rz", "art\\hud\\protohud.gfx", "scripts\\init.lua", "movies\\intro.bik",
    "audio\\boot_strap.p3d",
};
const char* const kQueries[] = {
    "art\\alex\\alex.p3d.rz", "\\art\\alex\\alex.p3d.rz", "\\\\art\\alex\\alex.p3d.rz", "ART\\ALEX\\ALEX.P3D.RZ",
    "scripts\\init.lua", "missing\\file.p3d", "", "\\",
};

}  // namespace

void TestCementLookup(protore::DiffResult* r) {
    TestFindEntry(r);

    auto original_find = reinterpret_cast<FindAndOpenFn>(protore::OriginalFunction(kFindAndOpenRva));
    auto original_open = reinterpret_cast<OpenByNameFn>(protore::OriginalFunction(kOpenByNameRva));
    auto* request = reinterpret_cast<core::CementOpenRequest*>(0x12340000);
    OpenRangeCapture capture;
    if (!capture.ok()) {
        protore::Check(r, false, "OpenRange capture unavailable");
        return;
    }
    std::vector<const char*> names(std::begin(kDirectoryNames), std::end(kDirectoryNames));
    for (int32_t state : {0, 1, 2}) {
        for (bool compact : {true, false}) {
            FakeLibrary lib(state, compact, names);
            for (const char* q : kQueries) {
                uint32_t key = core::HashFileName(q[0] == '\\' ? q + 1 : q);
                bool a = original_find(lib.library(), key, request);
                auto calls_a = capture.Take();
                bool b = core::CementLibraryFindAndOpen(lib.library(), key, request);
                auto calls_b = capture.Take();
                protore::Check(r, a == b, "FindAndOpen state %d compact %d \"%s\": %d != %d", state, compact, q, a, b);
                protore::Check(r, calls_a == calls_b, "FindAndOpen state %d compact %d \"%s\": OpenRange calls differ",
                               state, compact, q);

                a = original_open(lib.library(), q, request);
                calls_a = capture.Take();
                b = core::CementLibraryOpenByName(lib.library(), q, request);
                calls_b = capture.Take();
                protore::Check(r, a == b, "OpenByName state %d compact %d \"%s\": %d != %d", state, compact, q, a, b);
                protore::Check(r, calls_a == calls_b, "OpenByName state %d compact %d \"%s\": OpenRange calls differ",
                               state, compact, q);
            }
        }
    }
}

}  // namespace tests
