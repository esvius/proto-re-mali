#include "report_capture.h"

#include <cstdarg>
#include <cstdio>
#include <utility>

#include "../host/hooks.h"

namespace tests {

namespace {

constexpr uint32_t kVReportRva = 0x0063c340;
// mov ecx,[esp+14h]; mov eax,[esp+18h]
constexpr uint8_t kVReportBytes[] = {0x8b, 0x4c, 0x24, 0x14, 0x8b, 0x44, 0x24, 0x18};

std::vector<ReportCall> g_calls;
int g_scripted_result = 0;

std::string Str(const char* s) {
    return s ? std::string(s) : std::string("<null>");
}

int __cdecl RecordVReport(int kind, const char* tag, const char* file, int line, const char* expr,
                          const char* fmt, va_list args) {
    char message[1024] = "";
    if (fmt) {
        vsnprintf(message, sizeof(message), fmt, args);
    }
    g_calls.push_back({kind, Str(tag), Str(file), line, Str(expr), message});
    return g_scripted_result;
}

}  // namespace

bool operator==(const ReportCall& a, const ReportCall& b) {
    return a.kind == b.kind && a.tag == b.tag && a.file == b.file && a.line == b.line && a.expr == b.expr &&
           a.message == b.message;
}

ReportCapture::ReportCapture(int scripted_result) {
    g_calls.clear();
    g_scripted_result = scripted_result;
    detour_ = std::make_unique<protore::TestDetour>(kVReportRva, sizeof(kVReportBytes), kVReportBytes,
                                                    sizeof(kVReportBytes), reinterpret_cast<void*>(&RecordVReport));
}

ReportCapture::~ReportCapture() = default;

bool ReportCapture::active() const {
    return detour_ && detour_->ok();
}

std::vector<ReportCall> ReportCapture::Take() {
    return std::exchange(g_calls, {});
}

}  // namespace tests
