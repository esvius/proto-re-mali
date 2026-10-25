// Shared pieces of the Win32 emulation layer of the native host: kernel object handles, paths, last error.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "guest.h"

namespace native {

// ---- kernel objects ----
enum class KType { Event, Semaphore, Mutex, Thread, File, Find, Module };

// A thread blocked in WaitForSingleObject/WaitForMultipleObjects.
struct KWaiter {
    std::condition_variable cv;
};

struct KObject {
    explicit KObject(KType t) : type(t) {}
    virtual ~KObject() = default;
    // Waitable objects: signaled state and the side effect of a successful wait (auto-reset, count, ownership).
    virtual bool Signaled(uint32_t /*thread_id*/) { return false; }
    virtual void Acquire(uint32_t /*thread_id*/) {}
    // Wakes the threads waiting for this object; g_kernel_mutex is held.
    void Notify() {
        for (KWaiter* w : waiters) w->cv.notify_one();
    }
    KType type;
    int refs = 1;
    std::vector<KWaiter*> waiters;  // under g_kernel_mutex
};

// All waitable state changes happen under this lock and Notify() the object (g_kernel_cv: thread resumption).
extern std::mutex g_kernel_mutex;
extern std::condition_variable g_kernel_cv;

uint32_t NewHandle(KObject* obj);
KObject* HandleObject(uint32_t handle);  // null for invalid handles
void CloseKernelHandle(uint32_t handle);

constexpr uint32_t kInvalidHandle = 0xffffffffu;
constexpr uint32_t kCurrentProcess = 0xffffffffu;
constexpr uint32_t kCurrentThread = 0xfffffffeu;

// ---- errors ----
void SetLastError(uint32_t code);
uint32_t GetLastError();

// ---- paths ----
// Windows path (relative to the guest current directory, or absolute with a drive) -> host path. Components are
// matched case-insensitively against existing files, as on Windows.
std::string HostPath(const char* win_path);
// The guest current directory, "C:\Game" style; the game directory is C:\Game.
std::string GuestCurrentDir();
void SetGuestCurrentDir(const std::string& dir);
extern const char* const kGuestGameDir;  // "C:\\Game"

// ---- fibers (thread.cpp) ----
uint32_t FiberConvertThread(uint32_t param);
uint32_t FiberCreate(uint32_t stack_size, uint32_t start, uint32_t param);
void FiberSwitch(uint32_t handle);
void FiberDelete(uint32_t handle);
void FiberConvertToThread();
uint32_t TebTlsSlot(int index);

// ---- loaded modules ----
extern LoadedImage g_engine;

}  // namespace native
