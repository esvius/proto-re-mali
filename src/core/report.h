// core diagnostic reporting: the single entry point behind the engine's asserts ("oldassert") and
// debug traces (" Pre ", " Math  Pre "). 231 call sites in prototypeenginef.dll.
#pragma once

#include <cstdarg>

namespace core {

// Report kinds observed at call sites: 0 = trace (expr is null, fmt carries the message),
// 1 = assert (expr is the failed condition, fmt is usually null).
enum ReportKind { kReportTrace = 0, kReportAssert = 1 };

// Return value 3 tells the call site to clear its "enabled" flag and never report again.
constexpr int kReportDisableSite = 3;

// Original: FUN_1063c750 (rva 0x0063c750), cdecl, varargs forwarder to VReport.
int Report(int kind, const char* tag, const char* file, int line, const char* expr, const char* fmt, ...);

// Original: FUN_1063c340 (rva 0x0063c340), not reconstructed yet; called through the engine.
int VReport(int kind, const char* tag, const char* file, int line, const char* expr, const char* fmt,
            va_list args);

}  // namespace core
