#include <cstdint>
#include <cstring>
#include <vector>

#include "../core/cement_library.h"
#include "../host/hooks.h"
#include "tests.h"

namespace tests {

namespace {

constexpr uint32_t kValidateHeaderRva = 0x006503c0;
constexpr uint32_t kSetStateRva = 0x00650410;

using ValidateFn = bool(__thiscall*)(core::CementLibrary*);
using SetStateFn = void(__thiscall*)(core::CementLibrary*, int32_t);

constexpr char kMagic[] = "ATG CORE CEMENT LIBRARY";

struct Blob {
    alignas(4) uint8_t bytes[0x80];
    core::CementLibrary* library() { return reinterpret_cast<core::CementLibrary*>(bytes); }
};

// ---- completion object recording SetState's notifications ----
struct FakeCompletion {
    void** vtable;
    uint32_t id;
};

struct CompletionCall {
    uint32_t id;
    int slot;
    uint32_t arg;
    bool operator==(const CompletionCall& o) const { return id == o.id && slot == o.slot && arg == o.arg; }
};

std::vector<CompletionCall> g_calls;

void __thiscall CompletionNotify(FakeCompletion* c, uint32_t arg) {
    g_calls.push_back({c->id, 3, arg});
}
void __thiscall CompletionRelease(FakeCompletion* c) {
    g_calls.push_back({c->id, 1, 0});
}
void __thiscall CompletionUnexpected(FakeCompletion* c) {
    g_calls.push_back({c->id, -1, 0});
}

void* g_completion_vtable[8];

}  // namespace

void TestCementState(protore::DiffResult* r) {
    auto original_validate = reinterpret_cast<ValidateFn>(protore::OriginalFunction(kValidateHeaderRva));
    auto original_set_state = reinterpret_cast<SetStateFn>(protore::OriginalFunction(kSetStateRva));

    // ValidateHeader over every combination of the checked bytes and a few magic corruptions.
    const int magic_variants = 4;  // 0 = valid, 1 = first byte, 2 = last char, 3 = terminating NUL
    for (int magic = 0; magic < magic_variants; ++magic) {
        for (uint8_t major : {1, 2, 3}) {
            for (uint8_t minor : {0, 1, 2, 0xff}) {
                for (uint8_t reserved : {0, 1}) {
                    for (uint8_t flag : {0, 1, 0x80}) {
                        for (bool wide_equal : {true, false}) {
                            Blob blob;
                            memset(blob.bytes, 0x5a, sizeof(blob.bytes));
                            memcpy(blob.bytes + 0x28, kMagic, sizeof(kMagic));
                            if (magic == 1) blob.bytes[0x28] ^= 1;
                            if (magic == 2) blob.bytes[0x28 + 22] ^= 1;
                            if (magic == 3) blob.bytes[0x28 + 23] = 'X';
                            blob.bytes[0x48] = major;
                            blob.bytes[0x49] = minor;
                            blob.bytes[0x4a] = reserved;
                            blob.bytes[0x4b] = flag;
                            uint32_t a = 0x11111111, b = wide_equal ? a : 0x22222222;
                            memcpy(blob.bytes + 0x18, &a, 4);
                            memcpy(blob.bytes + 0x1c, &b, 4);
                            bool x = original_validate(blob.library());
                            bool y = core::CementLibraryValidateHeader(blob.library());
                            protore::Check(r, x == y,
                                           "ValidateHeader magic %d ver %u.%u.%u.%u wide_equal %d: %d != %d", magic,
                                           major, minor, reserved, flag, wide_equal, x, y);
                        }
                    }
                }
            }
        }
    }

    // SetState with and without a pending completion object.
    for (void*& slot : g_completion_vtable) {
        slot = reinterpret_cast<void*>(&CompletionUnexpected);
    }
    g_completion_vtable[1] = reinterpret_cast<void*>(&CompletionRelease);
    g_completion_vtable[3] = reinterpret_cast<void*>(&CompletionNotify);
    FakeCompletion completion{g_completion_vtable, 0xc0};
    for (bool pending : {false, true}) {
        for (int32_t state : {0, 1, 2, -1}) {
            Blob a, b;
            memset(a.bytes, 0x5a, sizeof(a.bytes));
            void* completion_ptr = pending ? &completion : nullptr;
            memcpy(a.bytes + 0x70, &completion_ptr, 4);
            uint32_t arg = 0xa5a5a5a5;
            memcpy(a.bytes + 0x74, &arg, 4);
            memcpy(b.bytes, a.bytes, sizeof(a.bytes));

            g_calls.clear();
            original_set_state(a.library(), state);
            std::vector<CompletionCall> calls_a;
            calls_a.swap(g_calls);
            core::CementLibrarySetState(b.library(), state);
            std::vector<CompletionCall> calls_b;
            calls_b.swap(g_calls);
            protore::Check(r, memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0, "SetState pending %d state %d: object differs",
                           pending, state);
            protore::Check(r, calls_a == calls_b, "SetState pending %d state %d: completion calls differ (%u vs %u)",
                           pending, state, static_cast<unsigned>(calls_a.size()), static_cast<unsigned>(calls_b.size()));
        }
    }
}

}  // namespace tests
