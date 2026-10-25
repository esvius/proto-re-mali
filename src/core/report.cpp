#include "report.h"

#include <cstdio>

#include "../host/hooks.h"

namespace core {

namespace {

constexpr uint32_t kVReportRva = 0x0063c340;

using VReportFn = int(__cdecl*)(int, const char*, const char*, int, const char*, const char*, va_list);

// Host-side side channel: mirror every report into proto-re.log so failing reconstructions show up as
// engine asserts with file:line instead of silent misbehaviour.
void Mirror(int kind, const char* tag, const char* file, int line, const char* expr, const char* fmt,
            va_list args) {
    char message[512] = "";
    if (fmt) {
        va_list copy;
        va_copy(copy, args);
        vsnprintf(message, sizeof(message), fmt, copy);
        va_end(copy);
    }
    protore::Log("report kind=%d tag=[%s] %s:%d expr=[%s] %s", kind, tag ? tag : "", file ? file : "?", line,
                 expr ? expr : "", message);
}

}  // namespace

int VReport(int kind, const char* tag, const char* file, int line, const char* expr, const char* fmt,
            va_list args) {
    auto original = reinterpret_cast<VReportFn>(protore::EngineAddress(kVReportRva));
    return original(kind, tag, file, line, expr, fmt, args);
}

int Report(int kind, const char* tag, const char* file, int line, const char* expr, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Mirror(kind, tag, file, line, expr, fmt, args);
    int result = VReport(kind, tag, file, line, expr, fmt, args);
    va_end(args);
    return result;
}

}  // namespace core
