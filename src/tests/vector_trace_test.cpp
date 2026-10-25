#include <cmath>
#include <cstdint>
#include <vector>

#include "../core/report.h"
#include "../host/hooks.h"
#include "../math/vector_trace.h"
#include "report_capture.h"
#include "tests.h"

namespace tests {

namespace {

using TraceVectorFn = void(__fastcall*)(const float*);

constexpr uint32_t kTraceVectorRva = 0x007cea20;
constexpr uint32_t kFloatFlagRva = 0x011bb9cd;
constexpr uint32_t kHexFlagRva = 0x011bb9cc;

struct Flags {
    char float_flag;
    char hex_flag;
};

volatile char* Flag(uint32_t rva) {
    return static_cast<volatile char*>(protore::EngineAddress(rva));
}

Flags ReadFlags() {
    return {*Flag(kFloatFlagRva), *Flag(kHexFlagRva)};
}

void WriteFlags(Flags f) {
    *Flag(kFloatFlagRva) = f.float_flag;
    *Flag(kHexFlagRva) = f.hex_flag;
}

}  // namespace

void TestTraceVector(protore::DiffResult* r) {
    auto original = reinterpret_cast<TraceVectorFn>(protore::OriginalFunction(kTraceVectorRva));
    const Flags saved = ReadFlags();
    const float inputs[][3] = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, -2.5f, 3.25f},
        {1e-30f, -1e30f, INFINITY},
        {-0.0f, NAN, 123456.789f},
    };
    const Flags flag_cases[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int scripted : {0, core::kReportDisableSite}) {
        for (const Flags& flags : flag_cases) {
            for (const auto& v : inputs) {
                ReportCapture capture(scripted);
                if (!capture.active()) {
                    protore::Check(r, false, "VReport capture unavailable");
                    WriteFlags(saved);
                    return;
                }
                WriteFlags(flags);
                original(v);
                Flags original_flags = ReadFlags();
                std::vector<ReportCall> original_calls = capture.Take();

                WriteFlags(flags);
                math::TraceVector(v);
                Flags reconstructed_flags = ReadFlags();
                std::vector<ReportCall> reconstructed_calls = capture.Take();

                protore::Check(r, original_calls == reconstructed_calls,
                               "flags %d/%d scripted %d v=(%g,%g,%g): %u vs %u report calls differ", flags.float_flag,
                               flags.hex_flag, scripted, v[0], v[1], v[2],
                               static_cast<unsigned>(original_calls.size()),
                               static_cast<unsigned>(reconstructed_calls.size()));
                protore::Check(r,
                               original_flags.float_flag == reconstructed_flags.float_flag &&
                                   original_flags.hex_flag == reconstructed_flags.hex_flag,
                               "flags %d/%d scripted %d: flag state after call differs", flags.float_flag,
                               flags.hex_flag, scripted);
            }
        }
    }
    WriteFlags(saved);
}

}  // namespace tests
