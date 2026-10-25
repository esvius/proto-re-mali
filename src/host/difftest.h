// Differential tests: run the original engine function and its reconstruction on the same inputs inside
// the game process and compare return values, observed outgoing calls and memory side effects.
// Enabled with PROTORE_DIFFTEST=1 (run after EngineInitialize, keep playing) or PROTORE_DIFFTEST=exit
// (run, then terminate the process with exit code 0 = all passed, 1 = failures).
#pragma once

namespace protore {

struct DiffResult {
    int checks = 0;
    int failures = 0;
};

using DiffTestFn = void (*)(DiffResult* result);

struct DiffTest {
    const char* name;
    DiffTestFn fn;
};

// Records one comparison; logs a description of every failure.
void Check(DiffResult* result, bool ok, const char* fmt, ...);

// Runs every registered test, logs a summary, returns the number of failed checks.
int RunDiffTests();

// Detours EngineInitialize so tests run once the engine is initialised. Called from DllMain.
void InstallDiffTestTrigger();

}  // namespace protore
