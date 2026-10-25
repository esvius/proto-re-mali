// Host-side entry points of the recompiled-code bridge (prototype inside the original 32-bit process).
#pragma once

#include <cstdint>

namespace protore {

// Entry stub that runs the recompiled version of the engine function at `va`, or null when it was not recompiled.
void* RecompEntryStub(uint32_t va);

// PROTORE_RECOMP=all: redirect every enterable engine function to its recompiled version. Returns the count.
int InstallAllRecompEntries();

}  // namespace protore
