// KERNEL32 for the native host: the subset prototypeenginef.dll imports.
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>
#include <unordered_map>
#include <vector>

#include "win.h"

namespace native {

std::mutex g_kernel_mutex;
std::condition_variable g_kernel_cv;
LoadedImage g_engine;
const char* const kGuestGameDir = "C:\\Game";

namespace {

// ---- handles ----
// handle = 0x100 + 4 * index. Lookups (every wait and signal) read the table without a lock; slots are assigned
// and cleared under g_handles_mutex.
constexpr uint32_t kMaxHandles = 1u << 16;
std::mutex g_handles_mutex;
std::atomic<KObject*> g_handles[kMaxHandles];
uint32_t g_handles_used = 0;  // slots below this have been handed out at least once

thread_local uint32_t tl_last_error = 0;

}  // namespace

uint32_t NewHandle(KObject* obj) {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    uint32_t i = 0;
    while (i < g_handles_used && g_handles[i].load(std::memory_order_relaxed)) ++i;
    if (i == kMaxHandles) Fatal("out of kernel handles");
    if (i == g_handles_used) ++g_handles_used;
    g_handles[i].store(obj, std::memory_order_release);
    return 0x100 + 4 * i;
}

KObject* HandleObject(uint32_t handle) {
    if (handle < 0x100 || (handle & 3)) return nullptr;
    uint32_t i = (handle - 0x100) / 4;
    return i < kMaxHandles ? g_handles[i].load(std::memory_order_acquire) : nullptr;
}

void CloseKernelHandle(uint32_t handle) {
    KObject* obj = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_handles_mutex);
        if (handle < 0x100 || (handle & 3)) return;
        uint32_t i = (handle - 0x100) / 4;
        if (i >= kMaxHandles) return;
        obj = g_handles[i].exchange(nullptr);
    }
    if (obj && --obj->refs == 0 && obj->type != KType::Thread) delete obj;
}

void SetLastError(uint32_t code) { tl_last_error = code; }
uint32_t GetLastError() { return tl_last_error; }

// ---- paths ----
namespace {

std::mutex g_cwd_mutex;
std::string g_cwd = kGuestGameDir;

std::string HostRootFor(char drive) {
    // C:\Game is the game directory; everything else on C: lives under native_c (saves, settings): in the app's
    // data folder on Android (the game folder may be shared storage), next to the game folder elsewhere.
    (void)drive;
    return g_data_dir ? std::string(g_data_dir) + "/native_c" : std::string(g_game_dir) + "/../native_c";
}

// Finds `name` in `dir` ignoring case; returns the real name or `name` itself when nothing matches.
std::string MatchCase(const std::string& dir, const std::string& name) {
    struct stat st;
    if (stat((dir + "/" + name).c_str(), &st) == 0) return name;
    DIR* d = opendir(dir.c_str());
    if (!d) return name;
    std::string found = name;
    while (dirent* e = readdir(d)) {
        if (strcasecmp(e->d_name, name.c_str()) == 0) {
            found = e->d_name;
            break;
        }
    }
    closedir(d);
    return found;
}

std::string Normalize(const std::string& path) {
    // Absolute Windows path with "." and ".." resolved, backslashes.
    std::vector<std::string> parts;
    std::string drive = path.substr(0, 2);
    size_t i = 2;
    while (i <= path.size()) {
        size_t j = path.find_first_of("\\/", i);
        if (j == std::string::npos) j = path.size();
        std::string p = path.substr(i, j - i);
        if (p == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!p.empty() && p != ".") {
            parts.push_back(p);
        }
        i = j + 1;
    }
    std::string r = drive;
    for (auto& p : parts) r += "\\" + p;
    if (parts.empty()) r += "\\";
    return r;
}

}  // namespace

std::string GuestCurrentDir() {
    std::lock_guard<std::mutex> lock(g_cwd_mutex);
    return g_cwd;
}

void SetGuestCurrentDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_cwd_mutex);
    g_cwd = dir;
}

std::string FullGuestPath(const char* win_path) {
    std::string p(win_path);
    if (p.size() >= 2 && p[1] == ':') return Normalize(p);
    if (!p.empty() && (p[0] == '\\' || p[0] == '/')) return Normalize(GuestCurrentDir().substr(0, 2) + p);
    return Normalize(GuestCurrentDir() + "\\" + p);
}

std::string HostPath(const char* win_path) {
    std::string full = FullGuestPath(win_path);
    std::string rest;
    std::string host;
    std::string game = kGuestGameDir;
    if (strncasecmp(full.c_str(), game.c_str(), game.size()) == 0 &&
        (full.size() == game.size() || full[game.size()] == '\\')) {
        host = g_game_dir;
        rest = full.substr(game.size());
    } else {
        host = HostRootFor(full[0]);
        rest = full.substr(2);
    }
    size_t i = 0;
    while (i < rest.size()) {
        if (rest[i] == '\\') {
            ++i;
            continue;
        }
        size_t j = rest.find('\\', i);
        if (j == std::string::npos) j = rest.size();
        host += "/" + MatchCase(host, rest.substr(i, j - i));
        i = j;
    }
    return host;
}

// Read statistics for the perf line: the engine streams music and speech from the archives with synchronous reads
// on its I/O thread, so slow storage (shared storage goes through FUSE) shows up as audio dropouts.
struct IoStats {
    std::atomic<uint64_t> reads{0}, bytes{0}, slow{0}, max_us{0};
} g_io;

// "N reads, M MiB, S over 50 ms, longest X ms" since the last call.
std::string TakeIoStats() {
    char line[128];
    snprintf(line, sizeof(line), "%llu reads, %llu MiB, %llu over 50 ms, longest %.1f ms",
             (unsigned long long)g_io.reads.exchange(0), (unsigned long long)(g_io.bytes.exchange(0) >> 20),
             (unsigned long long)g_io.slow.exchange(0), double(g_io.max_us.exchange(0)) / 1000.0);
    return line;
}

namespace {

// ---- waitable objects ----
struct Event : KObject {
    Event(bool manual, bool state) : KObject(KType::Event), manual(manual), state(state) {}
    bool Signaled(uint32_t) override { return state; }
    void Acquire(uint32_t) override {
        if (!manual) state = false;
    }
    bool manual, state;
};

struct Semaphore : KObject {
    Semaphore(int32_t count, int32_t max) : KObject(KType::Semaphore), count(count), max(max) {}
    bool Signaled(uint32_t) override { return count > 0; }
    void Acquire(uint32_t) override { --count; }
    int32_t count, max;
};

struct Mutex : KObject {
    Mutex() : KObject(KType::Mutex) {}
    bool Signaled(uint32_t tid) override { return owner == 0 || owner == tid; }
    void Acquire(uint32_t tid) override {
        owner = tid;
        ++recursion;
    }
    uint32_t owner = 0;
    int recursion = 0;
};

struct Thread : KObject {
    Thread() : KObject(KType::Thread) {}
    bool Signaled(uint32_t) override { return done; }
    bool done = false;
    bool suspended = false;
    uint32_t exit_code = 259;  // STILL_ACTIVE
    uint32_t id = 0;
    int host_tid = 0;
};

uint32_t CurrentTid() { return CurrentThread()->id; }

// Waits until `pred` holds or the timeout expires (0xffffffff = forever); g_kernel_mutex is held by `lock`. The
// thread sleeps on its own condition variable, registered with each object it waits for, so a signal wakes only
// that object's waiters (the engine's job system signals and waits tens of thousands of times a second).
template <typename Pred>
bool WaitKernel(std::unique_lock<std::mutex>& lock, uint32_t ms, KObject* const* objs, size_t n, Pred pred) {
    if (pred()) return true;
    if (ms == 0) return false;
    thread_local KWaiter self;
    for (size_t i = 0; i < n; ++i)
        if (objs[i]) objs[i]->waiters.push_back(&self);
    bool ok;
    if (ms == 0xffffffffu) {
        self.cv.wait(lock, pred);
        ok = true;
    } else {
        ok = self.cv.wait_for(lock, std::chrono::milliseconds(ms), pred);
    }
    for (size_t i = 0; i < n; ++i) {
        if (!objs[i]) continue;
        auto& w = objs[i]->waiters;
        for (size_t k = 0; k < w.size(); ++k) {
            if (w[k] == &self) {
                w[k] = w.back();
                w.pop_back();
                break;
            }
        }
    }
    return ok;
}

// ---- files ----
struct File : KObject {
    File() : KObject(KType::File) {}
    ~File() override {
        if (fd >= 0) close(fd);
    }
    int fd = -1;
};

struct Find : KObject {
    Find() : KObject(KType::Find) {}
    std::string dir, pattern;
    std::vector<std::string> names;
    size_t next = 0;
};

constexpr uint32_t ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5,
                   ERROR_INVALID_HANDLE = 6, ERROR_NO_MORE_FILES = 18, ERROR_ALREADY_EXISTS = 183,
                   ERROR_FILE_EXISTS = 80;

uint32_t ErrnoToWin(int e) {
    switch (e) {
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_ALREADY_EXISTS;
        default: return 31;  // ERROR_GEN_FAILURE
    }
}

// 100 ns intervals since 1601-01-01.
uint64_t ToFileTime(const struct timespec& ts) {
    return (uint64_t(ts.tv_sec) + 11644473600ull) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
}

uint32_t Attributes(const struct stat& st) {
    uint32_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x80;  // DIRECTORY : NORMAL
    if (!(st.st_mode & S_IWUSR)) a |= 0x01;           // READONLY
    return a;
}

void FillFindData(uint32_t out, const std::string& dir, const std::string& name) {
    memset(Ptr<void>(out), 0, 320);
    struct stat st;
    if (stat((dir + "/" + name).c_str(), &st) == 0) {
        W32(out, Attributes(st));
        uint64_t ft = ToFileTime(st.st_mtim);
        W64(out + 4, ft);
        W64(out + 12, ft);
        W64(out + 20, ft);
        W32(out + 28, uint32_t(uint64_t(st.st_size) >> 32));
        W32(out + 32, uint32_t(st.st_size));
    }
    strncpy(Str(out + 44), name.c_str(), 259);
}

bool FindNextMatch(Find* f, uint32_t out) {
    while (f->next < f->names.size()) {
        const std::string& n = f->names[f->next++];
        if (fnmatch(f->pattern.c_str(), n.c_str(), FNM_CASEFOLD) == 0) {
            FillFindData(out, f->dir, n);
            return true;
        }
    }
    return false;
}

// ---- modules ----
std::mutex g_modules_mutex;
std::vector<std::string> g_module_names;  // fake module handle = 0x0e000000 + 0x10000 * index
constexpr uint32_t kModuleBase = 0x0e000000u;
constexpr uint32_t kExeBase = 0x00400000u;

std::string LowerName(const char* s) {
    std::string r(s);
    for (char& ch : r) ch = char(tolower((unsigned char)ch));
    size_t slash = r.find_last_of("\\/");
    if (slash != std::string::npos) r = r.substr(slash + 1);
    if (r.find('.') == std::string::npos) r += ".dll";
    return r;
}

uint32_t ModuleHandle(const std::string& name) {
    if (name == "prototypeenginef.dll") return g_engine.base;
    if (name == "prototypef.exe") return kExeBase;
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    for (size_t i = 0; i < g_module_names.size(); ++i) {
        if (g_module_names[i] == name) return kModuleBase + 0x10000 * uint32_t(i);
    }
    g_module_names.push_back(name);
    return kModuleBase + 0x10000 * uint32_t(g_module_names.size() - 1);
}

// Libraries the host provides (LoadLibrary succeeds for these only).
bool KnownLibrary(const std::string& name) {
    static const char* known[] = {"kernel32.dll", "user32.dll", "advapi32.dll", "msvcr80.dll", "d3d9.dll",
                                  "dsound.dll", "xinput1_3.dll", "winmm.dll", "ws2_32.dll", "binkw32.dll",
                                  "shell32.dll", "ole32.dll", "oleaut32.dll", "prototypeenginef.dll"};
    for (const char* k : known)
        if (name == k) return true;
    return false;
}

auto g_start = std::chrono::steady_clock::now();

uint64_t NowFileTime() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ToFileTime(ts);
}

void WriteSystemTime(uint32_t out, const struct tm& t, int ms) {
    W16(out + 0, uint16_t(t.tm_year + 1900));
    W16(out + 2, uint16_t(t.tm_mon + 1));
    W16(out + 4, uint16_t(t.tm_wday));
    W16(out + 6, uint16_t(t.tm_mday));
    W16(out + 8, uint16_t(t.tm_hour));
    W16(out + 10, uint16_t(t.tm_min));
    W16(out + 12, uint16_t(t.tm_sec));
    W16(out + 14, uint16_t(ms));
}

// ---- critical sections: host recursive mutexes keyed by the guest CRITICAL_SECTION address ----
std::mutex g_cs_mutex;
std::unordered_map<uint32_t, std::recursive_mutex*> g_cs;

std::recursive_mutex* CriticalSectionSlow(uint32_t cs) {
    std::lock_guard<std::mutex> lock(g_cs_mutex);
    auto& m = g_cs[cs];
    if (!m) m = new std::recursive_mutex();
    return m;
}

// The mapping never changes (DeleteCriticalSection keeps the mutex), so each thread caches its lookups.
std::recursive_mutex* CriticalSection(uint32_t cs) {
    thread_local std::unordered_map<uint32_t, std::recursive_mutex*> cache;
    auto& m = cache[cs];
    if (!m) m = CriticalSectionSlow(cs);
    return m;
}

// ---- TLS ----
std::mutex g_tls_mutex;
bool g_tls_used[64];

#define K(name) void k_##name(Ctx* c)

// ---------------- handlers ----------------
K(GetCurrentProcessId) { Return(c, 0x1000); }
K(GetCurrentThreadId) { Return(c, CurrentTid()); }
K(GetCurrentProcess) { Return(c, kCurrentProcess); }
K(GetCurrentThread) { Return(c, kCurrentThread); }
K(GetTickCount) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start);
    Return(c, uint32_t(ms.count()));
}
K(QueryPerformanceCounter) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start);
    W64(Arg(c, 0), uint64_t(ns.count() / 100));
    Return(c, 1, 1);
}
K(QueryPerformanceFrequency) {
    W64(Arg(c, 0), 10000000ull);
    Return(c, 1, 1);
}
K(GetSystemTimeAsFileTime) {
    W64(Arg(c, 0), NowFileTime());
    Return(c, 0, 1);
}
K(GetLocalTime) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm t;
    localtime_r(&tv.tv_sec, &t);
    WriteSystemTime(Arg(c, 0), t, int(tv.tv_usec / 1000));
    Return(c, 0, 1);
}
K(GetTimeZoneInformation) {
    memset(Ptr<void>(Arg(c, 0)), 0, 172);
    Return(c, 0, 1);  // TIME_ZONE_ID_UNKNOWN, bias 0
}
K(InterlockedCompareExchange) {
    uint32_t expected = Arg(c, 2);
    __atomic_compare_exchange_n(Ptr<uint32_t>(Arg(c, 0)), &expected, Arg(c, 1), false, __ATOMIC_SEQ_CST,
                                __ATOMIC_SEQ_CST);
    Return(c, expected, 3);
}
K(InterlockedExchange) { Return(c, __atomic_exchange_n(Ptr<uint32_t>(Arg(c, 0)), Arg(c, 1), __ATOMIC_SEQ_CST), 2); }
K(InterlockedDecrement) { Return(c, __atomic_sub_fetch(Ptr<uint32_t>(Arg(c, 0)), 1, __ATOMIC_SEQ_CST), 1); }

K(VirtualAlloc) {
    uint32_t addr = Arg(c, 0), size = Arg(c, 1);
    uint32_t r = PageAlloc(size, addr);
    if (!r) SetLastError(8);  // ERROR_NOT_ENOUGH_MEMORY
    Return(c, r, 4);
}
K(VirtualFree) {
    if (Arg(c, 2) & 0x8000) PageFree(Arg(c, 0));  // MEM_RELEASE; MEM_DECOMMIT keeps the pages
    Return(c, 1, 3);
}
K(GetProcessHeap) { Return(c, 0x00ee0000u); }
K(HeapFree) {
    native::HeapFree(Arg(c, 2));
    Return(c, 1, 3);
}
K(GlobalMemoryStatus) {
    uint32_t m = Arg(c, 0);
    W32(m + 0, 32);
    W32(m + 4, 30);
    W32(m + 8, 0x7fffffffu);   // total physical
    W32(m + 12, 0x60000000u);  // available physical
    W32(m + 16, 0x7fffffffu);
    W32(m + 20, 0x60000000u);
    W32(m + 24, 0x7ffe0000u);  // total virtual
    W32(m + 28, 0x60000000u);
    Return(c, 0, 1);
}

K(GetModuleHandleA) {
    uint32_t name = Arg(c, 0);
    Return(c, name ? ModuleHandle(LowerName(Str(name))) : kExeBase, 1);
}
K(LoadLibraryA) {
    std::string n = LowerName(Str(Arg(c, 0)));
    if (!KnownLibrary(n)) {
        Log("LoadLibraryA(%s): not provided", Str(Arg(c, 0)));
        SetLastError(126);  // ERROR_MOD_NOT_FOUND
        Return(c, 0, 1);
        return;
    }
    Return(c, ModuleHandle(n), 1);
}
K(FreeLibrary) { Return(c, 1, 1); }
K(GetProcAddress) {
    uint32_t module = Arg(c, 0), name = Arg(c, 1);
    std::string proc = name < 0x10000 ? "#" + std::to_string(name) : std::string(Str(name));
    uint32_t r = 0;
    if (module == g_engine.base) {
        r = FindExport(g_engine, proc.c_str());
    } else if (module >= kModuleBase && module < kModuleBase + 0x1000000) {
        std::string dll;
        {
            std::lock_guard<std::mutex> lock(g_modules_mutex);
            dll = g_module_names[(module - kModuleBase) / 0x10000];
        }
        r = ImportThunk(dll.c_str(), proc.c_str());
    }
    Return(c, r, 2);
}
K(GetModuleFileNameA) {
    uint32_t module = Arg(c, 0), buf = Arg(c, 1), size = Arg(c, 2);
    std::string p = std::string(kGuestGameDir) + (module == g_engine.base ? "\\prototypeenginef.dll" : "\\prototypef.exe");
    uint32_t n = uint32_t(p.size()) < size ? uint32_t(p.size()) : size - 1;
    memcpy(Str(buf), p.c_str(), n);
    Str(buf)[n] = 0;
    Return(c, n, 3);
}

K(GetVersionExA) {
    uint32_t v = Arg(c, 0);
    W32(v + 4, 6);      // Windows 7
    W32(v + 8, 1);
    W32(v + 12, 7601);
    W32(v + 16, 2);     // VER_PLATFORM_WIN32_NT
    strcpy(Str(v + 20), "Service Pack 1");
    Return(c, 1, 1);
}
K(IsDebuggerPresent) { Return(c, 0); }
K(SetErrorMode) { Return(c, 0, 1); }
K(SetUnhandledExceptionFilter) { Return(c, 0, 1); }
K(UnhandledExceptionFilter) { Return(c, 0, 1); }
K(OutputDebugStringA) {
    Log("debug: %s", Str(Arg(c, 0)));
    Return(c, 0, 1);
}
K(RaiseException) {
    uint32_t code = Arg(c, 0);
    if (code == 0x406d1388u) {  // MSVC SetThreadName convention: ignored
        Return(c, 0, 4);
        return;
    }
    Fatal("RaiseException %08x from %08x", code, R32(c->esp));
}
K(TerminateProcess) {
    Log("TerminateProcess(%u)", Arg(c, 1));
    _exit(int(Arg(c, 1)));
}
K(GetLastError) { Return(c, GetLastError()); }
K(FormatMessageA) {
    uint32_t buf = Arg(c, 4), size = Arg(c, 5);
    const char* msg = "error";
    if (Arg(c, 0) & 0x100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
        W32(buf, GuestStrdup(msg));
    } else if (size) {
        strncpy(Str(buf), msg, size);
    }
    Return(c, uint32_t(strlen(msg)), 7);
}

// ---- threads ----
K(CreateThread) {
    uint32_t stack = Arg(c, 1), start = Arg(c, 2), param = Arg(c, 3), flags = Arg(c, 4), tid_out = Arg(c, 5);
    auto* t = new Thread();
    t->refs = 2;
    t->suspended = (flags & 4) != 0;  // CREATE_SUSPENDED
    uint32_t handle = NewHandle(t);
    std::atomic<uint32_t> id{0};
    std::thread([t, stack, start, param, &id]() {
        GuestThread* g = AttachThread(stack);
        t->host_tid = int(gettid());
        t->id = g->id;
        id.store(g->id);
        {
            std::unique_lock<std::mutex> lock(g_kernel_mutex);
            g_kernel_cv.wait(lock, [t] { return !t->suspended; });
        }
        uint32_t code = GuestCall(start, {param});
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        t->exit_code = code;
        t->done = true;
        t->Notify();
    }).detach();
    while (!id.load()) std::this_thread::yield();
    if (tid_out) W32(tid_out, id.load());
    Return(c, handle, 6);
}
K(ResumeThread) {
    auto* t = static_cast<Thread*>(HandleObject(Arg(c, 0)));
    uint32_t prev = 0;
    if (t && t->type == KType::Thread) {
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        prev = t->suspended ? 1 : 0;
        t->suspended = false;
        g_kernel_cv.notify_all();
    }
    Return(c, prev, 1);
}
K(SuspendThread) {
    Log("SuspendThread(%08x) ignored", Arg(c, 0));
    Return(c, 0, 1);
}
// Windows priorities -> nice values: the engine raises its audio mixer and render threads; on a phone they compete
// with half a dozen busy workers, so the hint matters.
// Threads the engine raises above normal (its render, streaming and audio feeder threads) are kept off the slowest
// cores (by cpu_capacity): left to the scheduler, and with power hints lowering clocks, Snapdragon phones park them
// on the little cores, where the audio feeder falls behind (choppy sound). Not with battery_saver.
static void AvoidLittleCores(int tid) {
#ifdef __ANDROID__
    static const cpu_set_t* fast = []() -> const cpu_set_t* {
        if (SettingInt("battery_saver", 0)) return nullptr;
        int caps[64], min_cap = -1, n = 0;
        for (; n < 64; ++n) {
            char path[96];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", n);
            FILE* f = fopen(path, "r");
            if (!f) break;
            if (fscanf(f, "%d", &caps[n]) != 1) caps[n] = 0;
            fclose(f);
            if (min_cap < 0 || caps[n] < min_cap) min_cap = caps[n];
        }
        static cpu_set_t set;
        CPU_ZERO(&set);
        int count = 0;
        for (int i = 0; i < n; ++i) {
            if (caps[i] > min_cap) {
                CPU_SET(i, &set);
                ++count;
            }
        }
        return count ? &set : nullptr;
    }();
    if (fast) sched_setaffinity(pid_t(tid), sizeof(*fast), fast);
#else
    (void)tid;
#endif
}

K(SetThreadPriority) {
    uint32_t h = Arg(c, 0);
    int prio = int32_t(Arg(c, 1));
    int tid = 0;
    if (h == kCurrentThread) {
        tid = int(gettid());
    } else {
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        KObject* o = HandleObject(h);
        if (o && o->type == KType::Thread) tid = static_cast<Thread*>(o)->host_tid;
    }
    int nice = prio >= 15 ? -10 : prio <= -15 ? 10 : -2 * prio;  // TIME_CRITICAL / IDLE / -2..2
    if (tid) {
        int r = setpriority(PRIO_PROCESS, id_t(tid), nice);
        Log("SetThreadPriority(%d) -> nice %d on tid %d%s", prio, nice, tid, r == 0 ? "" : " (not permitted)");
        // THREAD_PRIORITY_HIGHEST: the engine's render thread; TIME_CRITICAL: its audio feeder
        if (prio == 2 || prio >= 15) PerfHintAddThread(tid);
        if (prio > 0) AvoidLittleCores(tid);
    }
    Return(c, 1, 2);
}
K(SetThreadAffinityMask) { Return(c, 1, 2); }
K(GetProcessAffinityMask) {
    W32(Arg(c, 1), 0xf);
    W32(Arg(c, 2), 0xf);
    Return(c, 1, 3);
}
K(GetExitCodeThread) {
    auto* t = static_cast<Thread*>(HandleObject(Arg(c, 0)));
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    W32(Arg(c, 1), t && t->type == KType::Thread ? t->exit_code : 0);
    Return(c, 1, 2);
}
K(TerminateThread) {
    Log("TerminateThread(%08x) ignored", Arg(c, 0));
    Return(c, 1, 2);
}
K(ExitThread) { Fatal("ExitThread(%u): not supported yet", Arg(c, 0)); }
K(Sleep) {
    uint32_t ms = Arg(c, 0);
    if (ms == 0) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    Return(c, 0, 1);
}

// ---- fibers ----
K(ConvertThreadToFiber) { Return(c, FiberConvertThread(Arg(c, 0)), 1); }
K(CreateFiber) { Return(c, FiberCreate(Arg(c, 0), Arg(c, 1), Arg(c, 2)), 3); }
K(SwitchToFiber) {
    uint32_t target = Arg(c, 0);
    // Pop the call frame first: the resumed fiber continues from its own frame, and this one resumes here later.
    Return(c, 0, 1);
    FiberSwitch(target);
}
K(DeleteFiber) {
    FiberDelete(Arg(c, 0));
    Return(c, 0, 1);
}
K(ConvertFiberToThread) {
    FiberConvertToThread();
    Return(c, 1);
}

// ---- TLS ----
K(TlsAlloc) {
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    for (int i = 0; i < 64; ++i) {
        if (!g_tls_used[i]) {
            g_tls_used[i] = true;
            Return(c, uint32_t(i));
            return;
        }
    }
    Return(c, 0xffffffffu);
}
K(TlsFree) {
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    if (Arg(c, 0) < 64) g_tls_used[Arg(c, 0)] = false;
    Return(c, 1, 1);
}
K(TlsGetValue) {
    SetLastError(0);
    Return(c, R32(TebTlsSlot(int(Arg(c, 0)))), 1);
}
K(TlsSetValue) {
    W32(TebTlsSlot(int(Arg(c, 0))), Arg(c, 1));
    Return(c, 1, 2);
}

// ---- synchronization ----
K(InitializeCriticalSection) {
    CriticalSection(Arg(c, 0));
    Return(c, 0, 1);
}
K(DeleteCriticalSection) { Return(c, 0, 1); }
K(EnterCriticalSection) {
    CriticalSection(Arg(c, 0))->lock();
    Return(c, 0, 1);
}
K(LeaveCriticalSection) {
    CriticalSection(Arg(c, 0))->unlock();
    Return(c, 0, 1);
}
K(TryEnterCriticalSection) { Return(c, CriticalSection(Arg(c, 0))->try_lock() ? 1 : 0, 1); }

K(CreateEventA) { Return(c, NewHandle(new Event(Arg(c, 1) != 0, Arg(c, 2) != 0)), 4); }
K(SetEvent) {
    auto* e = static_cast<Event*>(HandleObject(Arg(c, 0)));
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    if (e && e->type == KType::Event) {
        e->state = true;
        e->Notify();
    }
    Return(c, e ? 1 : 0, 1);
}
K(ResetEvent) {
    auto* e = static_cast<Event*>(HandleObject(Arg(c, 0)));
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    if (e && e->type == KType::Event) e->state = false;
    Return(c, e ? 1 : 0, 1);
}
K(CreateSemaphoreA) { Return(c, NewHandle(new Semaphore(int32_t(Arg(c, 1)), int32_t(Arg(c, 2)))), 4); }
K(ReleaseSemaphore) {
    auto* s = static_cast<Semaphore*>(HandleObject(Arg(c, 0)));
    std::lock_guard<std::mutex> lock(g_kernel_mutex);
    if (!s || s->type != KType::Semaphore) {
        Return(c, 0, 3);
        return;
    }
    if (Arg(c, 2)) W32(Arg(c, 2), uint32_t(s->count));
    s->count += int32_t(Arg(c, 1));
    s->Notify();
    Return(c, 1, 3);
}
K(CreateMutexA) {
    auto* m = new Mutex();
    if (Arg(c, 1)) m->Acquire(CurrentTid());
    Return(c, NewHandle(m), 3);
}
K(WaitForSingleObject) {
    KObject* o = HandleObject(Arg(c, 0));
    if (!o) {
        SetLastError(ERROR_INVALID_HANDLE);
        Return(c, 0xffffffffu, 2);
        return;
    }
    uint32_t tid = CurrentTid();
    std::unique_lock<std::mutex> lock(g_kernel_mutex);
    bool ok = WaitKernel(lock, Arg(c, 1), &o, 1, [&] { return o->Signaled(tid); });
    if (ok) o->Acquire(tid);
    Return(c, ok ? 0 : 0x102, 2);  // WAIT_OBJECT_0 / WAIT_TIMEOUT
}
K(WaitForMultipleObjects) {
    uint32_t n = Arg(c, 0), handles = Arg(c, 1), all = Arg(c, 2), ms = Arg(c, 3);
    std::vector<KObject*> objs;
    for (uint32_t i = 0; i < n; ++i) objs.push_back(HandleObject(R32(handles + 4 * i)));
    uint32_t tid = CurrentTid();
    uint32_t which = 0x102;
    std::unique_lock<std::mutex> lock(g_kernel_mutex);
    WaitKernel(lock, ms, objs.data(), objs.size(), [&] {
        if (all) {
            for (KObject* o : objs)
                if (!o || !o->Signaled(tid)) return false;
            for (KObject* o : objs) o->Acquire(tid);
            which = 0;
            return true;
        }
        for (uint32_t i = 0; i < objs.size(); ++i) {
            if (objs[i] && objs[i]->Signaled(tid)) {
                objs[i]->Acquire(tid);
                which = i;
                return true;
            }
        }
        return false;
    });
    Return(c, which, 4);
}
K(CloseHandle) {
    CloseKernelHandle(Arg(c, 0));
    Return(c, 1, 1);
}

// ---- files ----
K(CreateFileA) {
    const char* name = Str(Arg(c, 0));
    uint32_t access = Arg(c, 1), disposition = Arg(c, 4);
    std::string host = HostPath(name);
    int flags = 0;
    bool write = (access & 0x40000000u) || (access & 0x10000000u);  // GENERIC_WRITE / GENERIC_ALL
    bool read = (access & 0x80000000u) || (access & 0x10000000u) || !write;
    flags = read && write ? O_RDWR : write ? O_WRONLY : O_RDONLY;
    switch (disposition) {
        case 1: flags |= O_CREAT | O_EXCL; break;   // CREATE_NEW
        case 2: flags |= O_CREAT | O_TRUNC; break;  // CREATE_ALWAYS
        case 3: break;                              // OPEN_EXISTING
        case 4: flags |= O_CREAT; break;            // OPEN_ALWAYS
        case 5: flags |= O_TRUNC; break;            // TRUNCATE_EXISTING
    }
    struct stat st;
    bool existed = stat(host.c_str(), &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        SetLastError(ERROR_ACCESS_DENIED);
        Return(c, kInvalidHandle, 7);
        return;
    }
    if (flags & O_CREAT) {
        // Create missing parent directories under native_c (saves) lazily.
        std::string dir = host.substr(0, host.find_last_of('/'));
        mkdir(dir.c_str(), 0755);
    }
    int fd = open(host.c_str(), flags | O_CLOEXEC, 0644);
    static const bool trace = getenv("PROTORE_TRACE_FILES") != nullptr;
    if (trace) Log("CreateFileA(%s, %x, disp %u) -> %s", name, access, disposition, fd < 0 ? strerror(errno) : "ok");
    if (fd < 0) {
        SetLastError(ErrnoToWin(errno));
        Return(c, kInvalidHandle, 7);
        return;
    }
    auto* f = new File();
    f->fd = fd;
    SetLastError((disposition == 2 || disposition == 4) && existed ? ERROR_ALREADY_EXISTS : 0);
    Return(c, NewHandle(f), 7);
}
File* FileOf(uint32_t h) {
    KObject* o = HandleObject(h);
    return o && o->type == KType::File ? static_cast<File*>(o) : nullptr;
}
// OVERLAPPED: [8] Offset, [12] OffsetHigh, [16] hEvent. I/O completes synchronously.
void CompleteOverlapped(uint32_t ov, uint32_t bytes) {
    W32(ov, 0);
    W32(ov + 4, bytes);
    if (uint32_t ev = R32(ov + 16)) {
        auto* e = static_cast<Event*>(HandleObject(ev & ~1u));
        std::lock_guard<std::mutex> lock(g_kernel_mutex);
        if (e && e->type == KType::Event) {
            e->state = true;
            e->Notify();
        }
    }
}
K(ReadFile) {
    File* f = FileOf(Arg(c, 0));
    uint32_t buf = Arg(c, 1), n = Arg(c, 2), out = Arg(c, 3), ov = Arg(c, 4);
    if (!f) {
        SetLastError(ERROR_INVALID_HANDLE);
        Return(c, 0, 5);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    ssize_t r = ov ? pread(f->fd, Ptr<void>(buf), n, off_t(R32(ov + 8)) | (off_t(R32(ov + 12)) << 32))
                   : read(f->fd, Ptr<void>(buf), n);
    uint64_t us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    g_io.reads.fetch_add(1, std::memory_order_relaxed);
    if (r > 0) g_io.bytes.fetch_add(uint64_t(r), std::memory_order_relaxed);
    if (us > 50000) g_io.slow.fetch_add(1, std::memory_order_relaxed);
    for (uint64_t m = g_io.max_us.load(); us > m && !g_io.max_us.compare_exchange_weak(m, us);) {
    }
    if (r < 0) {
        SetLastError(ErrnoToWin(errno));
        Return(c, 0, 5);
        return;
    }
    if (out) W32(out, uint32_t(r));
    if (ov) CompleteOverlapped(ov, uint32_t(r));
    Return(c, 1, 5);
}
K(WriteFile) {
    File* f = FileOf(Arg(c, 0));
    uint32_t buf = Arg(c, 1), n = Arg(c, 2), out = Arg(c, 3), ov = Arg(c, 4);
    if (!f) {
        SetLastError(ERROR_INVALID_HANDLE);
        Return(c, 0, 5);
        return;
    }
    ssize_t r = ov ? pwrite(f->fd, Ptr<void>(buf), n, off_t(R32(ov + 8)) | (off_t(R32(ov + 12)) << 32))
                   : write(f->fd, Ptr<void>(buf), n);
    if (r < 0) {
        SetLastError(ErrnoToWin(errno));
        Return(c, 0, 5);
        return;
    }
    if (out) W32(out, uint32_t(r));
    if (ov) CompleteOverlapped(ov, uint32_t(r));
    Return(c, 1, 5);
}
K(SetFilePointer) {
    File* f = FileOf(Arg(c, 0));
    int64_t dist = int32_t(Arg(c, 1));
    uint32_t high = Arg(c, 2);
    if (high) dist = int64_t((uint64_t(R32(high)) << 32) | Arg(c, 1));
    off_t r = f ? lseek(f->fd, off_t(dist), int(Arg(c, 3))) : -1;  // FILE_BEGIN/CURRENT/END = SEEK_*
    if (r < 0) {
        SetLastError(f ? 131 : ERROR_INVALID_HANDLE);  // ERROR_NEGATIVE_SEEK
        Return(c, 0xffffffffu, 4);
        return;
    }
    if (high) W32(high, uint32_t(uint64_t(r) >> 32));
    SetLastError(0);
    Return(c, uint32_t(r), 4);
}
K(GetFileSize) {
    File* f = FileOf(Arg(c, 0));
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) {
        Return(c, 0xffffffffu, 2);
        return;
    }
    if (Arg(c, 1)) W32(Arg(c, 1), uint32_t(uint64_t(st.st_size) >> 32));
    Return(c, uint32_t(st.st_size), 2);
}
K(SetEndOfFile) {
    File* f = FileOf(Arg(c, 0));
    Return(c, f && ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0 ? 1 : 0, 1);
}
K(FlushFileBuffers) { Return(c, 1, 1); }
K(GetFileTime) {
    File* f = FileOf(Arg(c, 0));
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) {
        Return(c, 0, 4);
        return;
    }
    uint64_t t = ToFileTime(st.st_mtim);
    for (int i = 1; i <= 3; ++i)
        if (Arg(c, i)) W64(Arg(c, i), t);
    Return(c, 1, 4);
}
K(CompareFileTime) {
    uint64_t a = R64(Arg(c, 0)), b = R64(Arg(c, 1));
    Return(c, a < b ? 0xffffffffu : a > b ? 1 : 0, 2);
}
K(DeleteFileA) {
    int r = unlink(HostPath(Str(Arg(c, 0))).c_str());
    if (r) SetLastError(ErrnoToWin(errno));
    Return(c, r == 0, 1);
}
K(MoveFileA) {
    int r = rename(HostPath(Str(Arg(c, 0))).c_str(), HostPath(Str(Arg(c, 1))).c_str());
    if (r) SetLastError(ErrnoToWin(errno));
    Return(c, r == 0, 2);
}
K(CopyFileA) {
    std::string from = HostPath(Str(Arg(c, 0))), to = HostPath(Str(Arg(c, 1)));
    struct stat st;
    if (Arg(c, 2) && stat(to.c_str(), &st) == 0) {
        SetLastError(ERROR_FILE_EXISTS);
        Return(c, 0, 3);
        return;
    }
    FILE* in = fopen(from.c_str(), "rb");
    FILE* out = in ? fopen(to.c_str(), "wb") : nullptr;
    bool ok = in && out;
    char buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out) fclose(out);
    if (!ok) SetLastError(ERROR_FILE_NOT_FOUND);
    Return(c, ok, 3);
}
K(GetFileAttributesA) {
    struct stat st;
    if (stat(HostPath(Str(Arg(c, 0))).c_str(), &st) != 0) {
        SetLastError(ErrnoToWin(errno));
        Return(c, 0xffffffffu, 1);
        return;
    }
    Return(c, Attributes(st), 1);
}
K(SetFileAttributesA) { Return(c, 1, 2); }
K(CreateDirectoryA) {
    std::string host = HostPath(Str(Arg(c, 0)));
    int r = mkdir(host.c_str(), 0755);
    if (r) SetLastError(errno == EEXIST ? ERROR_ALREADY_EXISTS : ErrnoToWin(errno));
    Return(c, r == 0, 2);
}
K(RemoveDirectoryA) { Return(c, rmdir(HostPath(Str(Arg(c, 0))).c_str()) == 0, 1); }
K(FindFirstFileA) {
    std::string full = FullGuestPath(Str(Arg(c, 0)));
    size_t slash = full.find_last_of('\\');
    auto* f = new Find();
    f->dir = HostPath(full.substr(0, slash).c_str());
    f->pattern = full.substr(slash + 1);
    if (f->pattern == "*.*") f->pattern = "*";
    if (DIR* d = opendir(f->dir.c_str())) {
        while (dirent* e = readdir(d)) f->names.push_back(e->d_name);
        closedir(d);
    }
    if (!FindNextMatch(f, Arg(c, 1))) {
        delete f;
        SetLastError(ERROR_FILE_NOT_FOUND);
        Return(c, kInvalidHandle, 2);
        return;
    }
    Return(c, NewHandle(f), 2);
}
K(FindNextFileA) {
    KObject* o = HandleObject(Arg(c, 0));
    if (!o || o->type != KType::Find || !FindNextMatch(static_cast<Find*>(o), Arg(c, 1))) {
        SetLastError(ERROR_NO_MORE_FILES);
        Return(c, 0, 2);
        return;
    }
    Return(c, 1, 2);
}
K(FindClose) {
    CloseKernelHandle(Arg(c, 0));
    Return(c, 1, 1);
}
K(GetFullPathNameA) {
    std::string full = FullGuestPath(Str(Arg(c, 0)));
    uint32_t size = Arg(c, 1), buf = Arg(c, 2), part = Arg(c, 3);
    if (full.size() + 1 > size) {
        Return(c, uint32_t(full.size() + 1), 4);
        return;
    }
    strcpy(Str(buf), full.c_str());
    if (part) W32(part, buf + uint32_t(full.find_last_of('\\') + 1));
    Return(c, uint32_t(full.size()), 4);
}
K(GetCurrentDirectoryA) {
    std::string d = GuestCurrentDir();
    uint32_t size = Arg(c, 0), buf = Arg(c, 1);
    if (d.size() + 1 > size) {
        Return(c, uint32_t(d.size() + 1), 2);
        return;
    }
    strcpy(Str(buf), d.c_str());
    Return(c, uint32_t(d.size()), 2);
}
K(SetCurrentDirectoryA) {
    SetGuestCurrentDir(FullGuestPath(Str(Arg(c, 0))));
    Return(c, 1, 1);
}
K(GetDriveTypeA) { Return(c, 3, 1); }  // DRIVE_FIXED
K(GetLogicalDrives) { Return(c, 1u << 2); }
K(GetVolumeInformationA) {
    if (Arg(c, 1)) strncpy(Str(Arg(c, 1)), "PROTO", Arg(c, 2));
    if (Arg(c, 3)) W32(Arg(c, 3), 0x12345678u);
    if (Arg(c, 4)) W32(Arg(c, 4), 255);
    if (Arg(c, 5)) W32(Arg(c, 5), 0x3);
    if (Arg(c, 6)) strncpy(Str(Arg(c, 6)), "NTFS", Arg(c, 7));
    Return(c, 1, 8);
}
K(GetDiskFreeSpaceExA) {
    struct statvfs s;
    uint64_t free_bytes = statvfs(g_game_dir, &s) == 0 ? uint64_t(s.f_bavail) * s.f_frsize : (1ull << 34);
    for (int i = 1; i <= 3; ++i)
        if (Arg(c, i)) W64(Arg(c, i), free_bytes);
    Return(c, 1, 4);
}
K(DeviceIoControl) {
    SetLastError(1);  // ERROR_INVALID_FUNCTION
    Return(c, 0, 8);
}
K(OpenFile) {
    // OFSTRUCT-based open; the engine only probes for existence (OF_EXIST).
    struct stat st;
    bool exists = stat(HostPath(Str(Arg(c, 0))).c_str(), &st) == 0;
    Return(c, exists ? 1 : 0xffffffffu, 3);
}

// ---- locale / strings ----
K(MultiByteToWideChar) {
    uint32_t src = Arg(c, 2), out = Arg(c, 4), out_size = Arg(c, 5);
    int32_t n = int32_t(Arg(c, 3));
    if (n < 0) n = int32_t(strlen(Str(src))) + 1;
    if (out_size == 0) {
        Return(c, uint32_t(n), 6);
        return;
    }
    int32_t m = n < int32_t(out_size) ? n : int32_t(out_size);
    for (int32_t i = 0; i < m; ++i) W16(out + 2 * i, R8(src + i));  // Latin-1 code page subset
    Return(c, uint32_t(m), 6);
}
K(lstrlenW) {
    uint32_t s = Arg(c, 0), n = 0;
    if (s)
        while (R16(s + 2 * n)) ++n;
    Return(c, n, 1);
}
K(GetACP) { Return(c, 1252); }
K(GetThreadLocale) { Return(c, 1033); }
K(GetSystemDefaultLangID) { Return(c, 1033); }
K(GetLocaleInfoA) {
    uint32_t type = Arg(c, 1), buf = Arg(c, 2), size = Arg(c, 3);
    const char* v = "";
    switch (type & 0xffff) {
        case 0x0003: v = "ENU"; break;            // LOCALE_SABBREVLANGNAME
        case 0x0059: v = "en"; break;             // LOCALE_SISO639LANGNAME
        case 0x005a: v = "US"; break;             // LOCALE_SISO3166CTRYNAME
        case 0x000e: v = "."; break;              // LOCALE_SDECIMAL
        case 0x000f: v = ","; break;              // LOCALE_STHOUSAND
        case 0x1004: v = "1252"; break;           // LOCALE_IDEFAULTANSICODEPAGE
        default: v = "";
    }
    uint32_t n = uint32_t(strlen(v)) + 1;
    if (size) strncpy(Str(buf), v, size);
    Return(c, n, 4);
}

#undef K

#define E(name) {"kernel32.dll", #name, k_##name}
const ImportDef kKernel32[] = {
    E(GetCurrentProcessId), E(GetCurrentThreadId), E(GetCurrentProcess), E(GetCurrentThread), E(GetTickCount),
    E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(GetSystemTimeAsFileTime), E(GetLocalTime),
    E(GetTimeZoneInformation), E(InterlockedCompareExchange), E(InterlockedExchange), E(InterlockedDecrement),
    E(VirtualAlloc), E(VirtualFree), E(GetProcessHeap), E(HeapFree), E(GlobalMemoryStatus), E(GetModuleHandleA),
    E(LoadLibraryA), E(FreeLibrary), E(GetProcAddress), E(GetModuleFileNameA), E(GetVersionExA),
    E(IsDebuggerPresent), E(SetErrorMode), E(SetUnhandledExceptionFilter), E(UnhandledExceptionFilter),
    E(OutputDebugStringA), E(RaiseException), E(TerminateProcess), E(GetLastError), E(FormatMessageA),
    E(CreateThread), E(ResumeThread), E(SuspendThread), E(SetThreadPriority), E(SetThreadAffinityMask),
    E(GetProcessAffinityMask), E(GetExitCodeThread), E(TerminateThread), E(ExitThread), E(Sleep),
    E(ConvertThreadToFiber), E(CreateFiber), E(SwitchToFiber), E(DeleteFiber), E(ConvertFiberToThread),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), E(InitializeCriticalSection),
    E(DeleteCriticalSection), E(EnterCriticalSection), E(LeaveCriticalSection), E(TryEnterCriticalSection),
    E(CreateEventA), E(SetEvent), E(ResetEvent), E(CreateSemaphoreA), E(ReleaseSemaphore), E(CreateMutexA),
    E(WaitForSingleObject), E(WaitForMultipleObjects), E(CloseHandle), E(CreateFileA), E(ReadFile), E(WriteFile),
    E(SetFilePointer), E(GetFileSize), E(SetEndOfFile), E(FlushFileBuffers), E(GetFileTime), E(CompareFileTime),
    E(DeleteFileA), E(MoveFileA), E(CopyFileA), E(GetFileAttributesA), E(SetFileAttributesA), E(CreateDirectoryA),
    E(RemoveDirectoryA), E(FindFirstFileA), E(FindNextFileA), E(FindClose), E(GetFullPathNameA),
    E(GetCurrentDirectoryA), E(SetCurrentDirectoryA), E(GetDriveTypeA), E(GetLogicalDrives),
    E(GetVolumeInformationA), E(GetDiskFreeSpaceExA), E(DeviceIoControl), E(OpenFile), E(MultiByteToWideChar),
    E(lstrlenW), E(GetACP), E(GetThreadLocale), E(GetSystemDefaultLangID), E(GetLocaleInfoA),
};
#undef E
ImportRegistrar g_kernel32(kKernel32);

}  // namespace

}  // namespace native
