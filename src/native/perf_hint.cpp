// Android Dynamic Performance Framework (APerformanceHint, Android 13+): the main and render threads report how
// long each frame's work took against the frame budget, so the system raises CPU clocks only when a frame would
// miss it and lowers them when there is headroom - most of the battery a game burns goes to clocks above what it
// needs. Loaded at run time (libmain targets API 26); a no-op where unavailable. Setting perf_hint=0 disables it.
#include "guest.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#include <unistd.h>

#include <mutex>
#include <vector>

namespace native {

namespace {

struct APerformanceHintManager;
struct APerformanceHintSession;

struct Api {
    APerformanceHintManager* (*getManager)();
    APerformanceHintSession* (*createSession)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
    int (*updateTarget)(APerformanceHintSession*, int64_t);
    int (*reportActual)(APerformanceHintSession*, int64_t);
    int (*setThreads)(APerformanceHintSession*, const int32_t*, size_t);  // API 34
} g_api;

std::mutex g_mutex;
APerformanceHintSession* g_session = nullptr;
std::vector<int32_t> g_tids;
int64_t g_target_ns = 16666667;

}  // namespace

void PerfHintStart(int fps_target) {
    if (!SettingInt("perf_hint", 1)) return;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    g_api.getManager = reinterpret_cast<decltype(g_api.getManager)>(dlsym(lib, "APerformanceHint_getManager"));
    g_api.createSession = reinterpret_cast<decltype(g_api.createSession)>(dlsym(lib, "APerformanceHint_createSession"));
    g_api.updateTarget =
        reinterpret_cast<decltype(g_api.updateTarget)>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    g_api.reportActual =
        reinterpret_cast<decltype(g_api.reportActual)>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    g_api.setThreads = reinterpret_cast<decltype(g_api.setThreads)>(dlsym(lib, "APerformanceHint_setThreads"));
    if (!g_api.getManager || !g_api.createSession || !g_api.updateTarget || !g_api.reportActual) return;
    APerformanceHintManager* manager = g_api.getManager();
    if (!manager) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_target_ns = 1000000000ll / (fps_target > 0 ? fps_target : 60);
    g_tids.push_back(int32_t(gettid()));
    g_session = g_api.createSession(manager, g_tids.data(), g_tids.size(), g_target_ns);
    Log("perf hint: %s (target %.1f ms, %zu threads)", g_session ? "session" : "unavailable",
        double(g_target_ns) / 1e6, g_tids.size());
}

// A thread that works on every frame (the engine's render thread, the audio threads): part of the session
// (Android 14+).
// Threads may register before the session exists (the engine raises its render thread during EngineInitialize,
// before the main loop starts the session): they are kept and join it then.
void PerfHintAddThread(int tid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int32_t t : g_tids)
        if (t == tid) return;
    g_tids.push_back(int32_t(tid));
    if (g_session && g_api.setThreads) g_api.setThreads(g_session, g_tids.data(), g_tids.size());
}

void PerfHintTarget(int fps_target) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_target_ns = 1000000000ll / (fps_target > 0 ? fps_target : 60);
    if (g_session) g_api.updateTarget(g_session, g_target_ns);
}

void PerfHintFrame(int64_t work_ns) {
    if (g_session && work_ns > 0) g_api.reportActual(g_session, work_ns);
}

}  // namespace native

#else

namespace native {

void PerfHintStart(int) {}
void PerfHintAddThread(int) {}
void PerfHintTarget(int) {}
void PerfHintFrame(int64_t) {}

}  // namespace native

#endif
