#include "difftest.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../tests/tests.h"
#include "hooks.h"

namespace protore {

namespace {

constexpr uint32_t kEngineInitializeRva = 0x0085efb0;
// sub esp,0B78h
constexpr uint8_t kEngineInitializeBytes[] = {0x81, 0xec, 0x78, 0x0b, 0x00, 0x00};

Detour g_engine_initialize;
const char* g_current_test = "";

using EngineInitializeFn = int(__cdecl*)(void*, void*, void*, void*);

// EngineInitialize is cdecl and reads its arguments from the caller's frame; forwarding four stack slots
// covers every argument it reads without depending on the exact count.
int __cdecl EngineInitializeWrapper(void* a, void* b, void* c, void* d) {
    Log("difftest: EngineInitialize(%p, %p, %p, %p) via trampoline %p", a, b, c, d, g_engine_initialize.trampoline);
    int result = reinterpret_cast<EngineInitializeFn>(g_engine_initialize.trampoline)(a, b, c, d);
    Log("difftest: EngineInitialize returned %d", result);
    int failures = RunDiffTests();
    char mode[16] = "";
    GetEnvironmentVariableA("PROTORE_DIFFTEST", mode, sizeof(mode));
    if (strcmp(mode, "exit") == 0) {
        ExitProcess(failures ? 1 : 0);
    }
    return result;
}

}  // namespace

void Check(DiffResult* result, bool ok, const char* fmt, ...) {
    ++result->checks;
    if (ok) {
        return;
    }
    ++result->failures;
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    Log("difftest %s: FAIL %s", g_current_test, message);
}

int RunDiffTests() {
    size_t count = 0;
    const DiffTest* tests = tests::All(&count);
    int total_checks = 0;
    int total_failures = 0;
    for (size_t i = 0; i < count; ++i) {
        g_current_test = tests[i].name;
        Log("difftest %s: running", tests[i].name);
        DiffResult result;
        tests[i].fn(&result);
        Log("difftest %s: %d checks, %d failures", tests[i].name, result.checks, result.failures);
        total_checks += result.checks;
        total_failures += result.failures;
    }
    Log("difftest summary: %u tests, %d checks, %d failures", static_cast<unsigned>(count), total_checks,
        total_failures);
    return total_failures;
}

void InstallDiffTestTrigger() {
    if (!DetourAttach(&g_engine_initialize, kEngineInitializeRva, sizeof(kEngineInitializeBytes),
                      kEngineInitializeBytes, sizeof(kEngineInitializeBytes),
                      reinterpret_cast<void*>(&EngineInitializeWrapper))) {
        Log("difftest: could not detour EngineInitialize, tests will not run");
        return;
    }
    Log("difftest: armed, tests run after EngineInitialize");
}

}  // namespace protore
