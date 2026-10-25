// core platform primitives the engine wraps around Win32: critical sections and Sleep. These are the functions that
// get a POSIX implementation on Android; on Windows they forward to the same APIs the originals import.
#pragma once

#include <cstdint>

namespace core {

struct Mutex;  // a Win32 CRITICAL_SECTION (24 bytes) embedded in engine objects

// Originals (all cdecl, one argument): FUN_1063d260 init, FUN_1063d270 destroy, FUN_1063d290 lock,
// FUN_1063d280 unlock.
void MutexInit(Mutex* m);
void MutexDestroy(Mutex* m);
void MutexLock(Mutex* m);
void MutexUnlock(Mutex* m);

// Original: FUN_10644b40 (rva 0x00644b40), cdecl. Sleep(milliseconds); 0 yields the time slice.
void ThreadSleep(uint32_t milliseconds);

}  // namespace core
