#include <cstdint>
#include <string>
#include <vector>

#include "../core/name_hash.h"
#include "../host/hooks.h"
#include "tests.h"

namespace tests {

namespace {

constexpr uint32_t kHashStringRva = 0x0063a5d0;
constexpr uint32_t kHashFileNameRva = 0x00643ed0;
constexpr uint32_t kHashObserverRva = 0x010bc59c;

using HashStringFn = uint32_t(__cdecl*)(const char*, uint32_t);
using HashFileNameFn = uint32_t(__cdecl*)(const char*);
using ObserverFn = void(__cdecl*)(uint32_t, const char*);

struct ObservedCall {
    uint32_t hash;
    const char* s;
    bool operator==(const ObservedCall& o) const { return hash == o.hash && s == o.s; }
};

std::vector<ObservedCall> g_observed;

void __cdecl RecordObserver(uint32_t hash, const char* s) {
    g_observed.push_back({hash, s});
}

// Swaps the engine's hash observer for a recorder for the lifetime of the object.
class ObserverCapture {
public:
    ObserverCapture() : slot_(static_cast<ObserverFn*>(protore::EngineAddress(kHashObserverRva))), saved_(*slot_) {
        *slot_ = &RecordObserver;
        g_observed.clear();
    }
    ~ObserverCapture() { *slot_ = saved_; }
    std::vector<ObservedCall> Take() {
        std::vector<ObservedCall> out;
        out.swap(g_observed);
        return out;
    }

private:
    ObserverFn* slot_;
    ObserverFn saved_;
};

const char* const kInputs[] = {
    "",
    "a",
    "A",
    "\\art\\alex\\alex.p3d.rz",
    "art\\alex\\alex.p3d.rz",
    "\\\\double\\leading",
    "scripts\\frevents.lua",
    "MiXeD_Case-0123456789.~!@#$%^&*()[]{}",
    "\x7f\x80\x81\xc0\xe9\xff",  // high-bit bytes exercise the signed-char path
    "very/long/path/with/forward/slashes/and/many/segments/to/overflow/the/32/bit/accumulator/many/times.p3d",
};

}  // namespace

void TestNameHash(protore::DiffResult* r) {
    auto original_string = reinterpret_cast<HashStringFn>(protore::OriginalFunction(kHashStringRva));
    auto original_file = reinterpret_cast<HashFileNameFn>(protore::OriginalFunction(kHashFileNameRva));
    ObserverCapture capture;

    for (uint32_t seed : {0u, 1u, 0xdeadbeefu, 0xffffffffu}) {
        uint32_t a = original_string(nullptr, seed);
        uint32_t b = core::HashString(nullptr, seed);
        protore::Check(r, a == b, "HashString(null, %08x): %08x != %08x", seed, a, b);
        for (const char* s : kInputs) {
            a = original_string(s, seed);
            auto observed_a = capture.Take();
            b = core::HashString(s, seed);
            auto observed_b = capture.Take();
            protore::Check(r, a == b, "HashString(\"%s\", %08x): %08x != %08x", s, seed, a, b);
            protore::Check(r, observed_a == observed_b, "HashString(\"%s\", %08x): observer calls differ", s, seed);
        }
    }
    for (const char* s : kInputs) {
        uint32_t a = original_file(s);
        auto observed_a = capture.Take();
        uint32_t b = core::HashFileName(s);
        auto observed_b = capture.Take();
        protore::Check(r, a == b, "HashFileName(\"%s\"): %08x != %08x", s, a, b);
        protore::Check(r, observed_a == observed_b, "HashFileName(\"%s\"): observer calls differ", s);
    }
}

}  // namespace tests
