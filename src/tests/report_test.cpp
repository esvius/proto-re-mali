#include <vector>

#include "../core/report.h"
#include "../host/hooks.h"
#include "report_capture.h"
#include "tests.h"

namespace tests {

namespace {

using ReportFn = int(__cdecl*)(int, const char*, const char*, int, const char*, const char*, ...);

constexpr uint32_t kReportRva = 0x0063c750;

// Calls `fn` with one fixed set of arguments covering both report kinds and varargs of mixed width.
int CallCase(ReportFn fn, int which) {
    switch (which) {
        case 0:
            return fn(core::kReportTrace, " Pre ", "..\\code\\test.cpp", 12, nullptr, "[ %f, %f, %f ]", 1.5, -2.25,
                      1e10);
        case 1:
            return fn(core::kReportTrace, " Math  Pre ", "..\\code\\test.cpp", 13, nullptr, "%d %s %#x %c", -7,
                      "text", 0xdeadbeefu, 'z');
        case 2:
            return fn(core::kReportAssert, " oldassert ", "..\\code\\test.cpp", 14, "a == b", nullptr);
        default:
            return fn(core::kReportTrace, nullptr, nullptr, 0, nullptr, nullptr);
    }
}

}  // namespace

void TestReport(protore::DiffResult* r) {
    auto original = reinterpret_cast<ReportFn>(protore::OriginalFunction(kReportRva));
    auto reconstructed = static_cast<ReportFn>(&core::Report);
    for (int scripted : {0, 1, core::kReportDisableSite}) {
        for (int which = 0; which < 4; ++which) {
            ReportCapture capture(scripted);
            if (!capture.active()) {
                protore::Check(r, false, "VReport capture unavailable");
                return;
            }
            int original_result = CallCase(original, which);
            std::vector<ReportCall> original_calls = capture.Take();
            int reconstructed_result = CallCase(reconstructed, which);
            std::vector<ReportCall> reconstructed_calls = capture.Take();
            protore::Check(r, original_result == reconstructed_result, "case %d scripted %d: result %d != %d", which,
                           scripted, original_result, reconstructed_result);
            protore::Check(r, original_calls == reconstructed_calls, "case %d scripted %d: VReport calls differ",
                           which, scripted);
            protore::Check(r, original_calls.size() == 1, "case %d: original made %u VReport calls", which,
                           static_cast<unsigned>(original_calls.size()));
        }
    }
}

}  // namespace tests
