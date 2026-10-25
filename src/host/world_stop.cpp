#include "world_stop.h"

#include <windows.h>
#include <tlhelp32.h>

namespace protore {

WorldStop::WorldStop() {
    const DWORD self_process = GetCurrentProcessId();
    const DWORD self_thread = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self_process || entry.th32ThreadID == self_thread || count_ == kMaxThreads) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            CloseHandle(thread);
            continue;
        }
        threads_[count_++] = thread;
    }
    CloseHandle(snapshot);
}

WorldStop::~WorldStop() {
    while (count_) {
        HANDLE thread = threads_[--count_];
        ResumeThread(thread);
        CloseHandle(thread);
    }
}

}  // namespace protore
