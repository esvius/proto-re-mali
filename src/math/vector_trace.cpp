#include "vector_trace.h"

#include <cstdint>

#include "../core/report.h"
#include "../host/hooks.h"

namespace math {

namespace {

// Per-site trace enable flags in the engine's data section (cleared when Report returns kReportDisableSite).
constexpr uint32_t kTraceFloatFlagRva = 0x011bb9cd;
constexpr uint32_t kTraceHexFlagRva = 0x011bb9cc;

constexpr const char* kFile = "..\\..\\..\\code\\math\\src\\vector.cpp";

uint32_t FloatBits(float f) {
    union {
        float f;
        uint32_t u;
    } bits;
    bits.f = f;
    return bits.u;
}

}  // namespace

void __fastcall TraceVector(const float* v) {
    float x = v[0];
    float y = v[1];
    float z = v[2];
    auto* float_flag = static_cast<volatile char*>(protore::EngineAddress(kTraceFloatFlagRva));
    auto* hex_flag = static_cast<volatile char*>(protore::EngineAddress(kTraceHexFlagRva));
    if (*float_flag) {
        if (core::Report(core::kReportTrace, " Pre ", kFile, 0xd5, nullptr, "[ %f, %f, %f ]", double(x), double(y),
                         double(z), 0) == core::kReportDisableSite) {
            *float_flag = 0;
        }
    }
    if (*hex_flag) {
        if (core::Report(core::kReportTrace, " Pre ", kFile, 0xd6, nullptr, "[ %#x, %#x, %#x ]", FloatBits(x),
                         FloatBits(y), FloatBits(z), 0) == core::kReportDisableSite) {
            *hex_flag = 0;
        }
    }
}

}  // namespace math
