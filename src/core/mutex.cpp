#include "mutex.h"

#include <windows.h>

namespace core {

namespace {

CRITICAL_SECTION* Native(Mutex* m) {
    return reinterpret_cast<CRITICAL_SECTION*>(m);
}

}  // namespace

static_assert(sizeof(CRITICAL_SECTION) == 24, "engine objects embed a 24-byte critical section");

void MutexInit(Mutex* m) {
    InitializeCriticalSection(Native(m));
}

void MutexDestroy(Mutex* m) {
    DeleteCriticalSection(Native(m));
}

void MutexLock(Mutex* m) {
    EnterCriticalSection(Native(m));
}

void MutexUnlock(Mutex* m) {
    LeaveCriticalSection(Native(m));
}

void ThreadSleep(uint32_t milliseconds) {
    Sleep(milliseconds);
}

}  // namespace core
