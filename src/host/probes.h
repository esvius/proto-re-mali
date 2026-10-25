// Diagnostic probes: detours that only observe (log arguments and caller) and then continue into the
// original function. Selected with PROTORE_PROBE=<name>; used to locate code, never shipped as behaviour.
#pragma once

namespace protore {

void InstallProbes();

}  // namespace protore
