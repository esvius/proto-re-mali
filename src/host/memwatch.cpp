#include "memwatch.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "hooks.h"

namespace protore {

namespace {

constexpr size_t kMaxSites = 64;

std::vector<uint8_t> g_pattern;
size_t g_watch_len = 0;
volatile uintptr_t g_watch_begin = 0;
volatile uintptr_t g_watch_end = 0;
uintptr_t g_page_begin = 0;
size_t g_page_len = 0;

struct Site {
    uint32_t eip;
    uint32_t caller;  // return address on the stack at the time (best effort)
    uint32_t count;
};
Site g_sites[kMaxSites];
volatile LONG g_site_count = 0;
CRITICAL_SECTION g_sites_lock;

void Arm() {
    DWORD old_protect;
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(reinterpret_cast<void*>(g_page_begin), &info, sizeof(info));
    VirtualProtect(reinterpret_cast<void*>(g_page_begin), g_page_len, info.Protect | PAGE_GUARD, &old_protect);
}

void Record(uint32_t eip, uint32_t esp) {
    uint32_t caller = esp ? *reinterpret_cast<uint32_t*>(esp) : 0;
    EnterCriticalSection(&g_sites_lock);
    for (LONG i = 0; i < g_site_count; ++i) {
        if (g_sites[i].eip == eip) {
            ++g_sites[i].count;
            LeaveCriticalSection(&g_sites_lock);
            return;
        }
    }
    if (g_site_count < static_cast<LONG>(kMaxSites)) {
        g_sites[g_site_count++] = {eip, caller, 1};
        uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(EngineAddress(0)));
        Log("memwatch: new access site eip_rva=%08x stack_top_rva=%08x", eip - base, caller - base);
    }
    LeaveCriticalSection(&g_sites_lock);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION) {
        uintptr_t addr = info->ExceptionRecord->ExceptionInformation[1];
        if (addr < g_page_begin || addr >= g_page_begin + g_page_len) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (addr >= g_watch_begin && addr < g_watch_end) {
            Record(info->ContextRecord->Eip, info->ContextRecord->Esp);
        }
        info->ContextRecord->EFlags |= 0x100;  // single step, then re-arm
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == STATUS_SINGLE_STEP && g_page_len) {
        Arm();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

uintptr_t Scan() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION info;
    while (addr < end && VirtualQuery(reinterpret_cast<void*>(addr), &info, sizeof(info))) {
        uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        bool readable = info.State == MEM_COMMIT && info.Type == MEM_PRIVATE &&
                        (info.Protect & (PAGE_READWRITE | PAGE_READONLY)) && !(info.Protect & PAGE_GUARD);
        if (readable) {
            auto* p = static_cast<const uint8_t*>(info.BaseAddress);
            size_t n = info.RegionSize;
            for (size_t i = 0; i + g_pattern.size() <= n; i += 4) {
                if (p[i] == g_pattern[0] && memcmp(p + i, g_pattern.data(), g_pattern.size()) == 0 &&
                    p + i != g_pattern.data()) {
                    return reinterpret_cast<uintptr_t>(p + i);
                }
            }
        }
        addr = next;
    }
    return 0;
}

DWORD WINAPI WatchThread(LPVOID) {
    for (int attempt = 0; attempt < 600; ++attempt) {
        uintptr_t found = Scan();
        if (found) {
            g_watch_begin = found;
            g_watch_end = found + g_watch_len;
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            g_page_begin = found & ~uintptr_t(si.dwPageSize - 1);
            g_page_len = ((g_watch_end + si.dwPageSize - 1) & ~uintptr_t(si.dwPageSize - 1)) - g_page_begin;
            Log("memwatch: pattern found at %p after %d scans, watching %u bytes", reinterpret_cast<void*>(found),
                attempt + 1, static_cast<unsigned>(g_watch_len));
            Arm();
            return 0;
        }
        Sleep(50);
    }
    Log("memwatch: pattern not found");
    return 0;
}

}  // namespace

void StartMemoryWatch(const uint8_t* pattern, size_t pattern_len, size_t watch_len) {
    g_pattern.assign(pattern, pattern + pattern_len);
    g_watch_len = watch_len;
    InitializeCriticalSection(&g_sites_lock);
    AddVectoredExceptionHandler(1, &Handler);
    CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr);
}

void StartRcfDirectoryWatch() {
    FILE* f = fopen("art.rcf", "rb");
    if (!f) {
        Log("memwatch: cannot open art.rcf");
        return;
    }
    uint32_t header[7];
    fseek(f, 0x20, SEEK_SET);
    fread(header, sizeof(header), 1, f);
    uint32_t dir_offset = header[1];
    uint32_t dir_size = header[2];
    uint8_t pattern[48];
    fseek(f, static_cast<long>(dir_offset), SEEK_SET);
    fread(pattern, sizeof(pattern), 1, f);
    fclose(f);
    Log("memwatch: art.rcf directory at file offset 0x%x, %u bytes", dir_offset, dir_size);
    StartMemoryWatch(pattern, sizeof(pattern), dir_size);
}

}  // namespace protore
