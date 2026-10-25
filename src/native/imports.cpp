// Import thunks: each imported dll!name gets a 16-byte slot at g_thunk_base + 16*index (an arena block: a fixed
// address may be taken on Android, where ART maps its heap below 4 GiB). Generated code calls through
// the IAT with RecompDispatch(target), and target falls in this range. Data imports (MSVCR80 _HUGE, _adjust_fdiv)
// keep their value in the slot itself.
#include <sys/mman.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "guest.h"

namespace native {

namespace {

uint32_t g_thunk_base = 0;
constexpr uint32_t kThunkSize = 0x00100000u;
constexpr uint32_t kSlot = 16;

struct Slot {
    std::string dll, name;
    ImportFn fn;
};

std::map<std::string, ImportFn>& Handlers() {
    static std::map<std::string, ImportFn> handlers;
    return handlers;
}

std::vector<Slot>& Slots() {
    static std::vector<Slot> slots;
    return slots;
}

std::string Key(const char* dll, const char* name) { return std::string(dll) + "!" + name; }

// Setting import_profile: every thread logs, every 10 s, where its wall time went - the imports (WinAPI, D3D9
// methods) it spent the most time in, inclusive of nested calls, and the rest (guest code). Finds the waits that
// keep a frame long while no thread is busy.
bool ImportProfiling() {
    static const bool on = SettingInt("import_profile", 0) != 0;
    return on;
}

struct ThreadProfile {
    using Clock = std::chrono::steady_clock;
    Clock::time_point window = Clock::now();
    std::vector<uint64_t> ns, calls;
    int depth = 0;
};

void ProfiledCall(const Slot& s, uint32_t index, Ctx* ctx) {
    thread_local ThreadProfile tp;
    if (tp.ns.size() <= index) {
        tp.ns.resize(index + 64);
        tp.calls.resize(index + 64);
    }
    auto t0 = ThreadProfile::Clock::now();
    ++tp.depth;
    s.fn(ctx);
    --tp.depth;
    auto t1 = ThreadProfile::Clock::now();
    tp.ns[index] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    ++tp.calls[index];
    if (tp.depth || t1 - tp.window < std::chrono::seconds(10)) return;
    const double wall = std::chrono::duration<double, std::milli>(t1 - tp.window).count();
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < tp.ns.size(); ++i) {
        if (tp.calls[i]) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return tp.ns[a] > tp.ns[b]; });
    std::string line;
    double in_imports = 0;
    for (uint32_t i : order) in_imports += double(tp.ns[i]) / 1e6;
    auto& slots = Slots();
    for (size_t k = 0; k < order.size() && k < 10; ++k) {
        uint32_t i = order[k];
        char buf[160];
        snprintf(buf, sizeof(buf), " %s %.0f%% (%llu)", slots[i].name.c_str(), 100.0 * double(tp.ns[i]) / 1e6 / wall,
                 (unsigned long long)tp.calls[i]);
        line += buf;
    }
    Log("imports tid %d: guest %.0f%% |%s", int(gettid()), 100.0 * (wall - in_imports) / wall, line.c_str());
    std::fill(tp.ns.begin(), tp.ns.end(), 0);
    std::fill(tp.calls.begin(), tp.calls.end(), 0);
    tp.window = t1;
}

}  // namespace

void RegisterImports(const ImportDef* defs, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        Handlers()[Key(defs[i].dll, defs[i].name)] = defs[i].fn;
    }
}

uint32_t ImportThunk(const char* dll, const char* name) {
    if (!g_thunk_base) {
        g_thunk_base = PageAlloc(kThunkSize);
        if (!g_thunk_base) Fatal("cannot allocate the import thunk area");
    }
    std::string key = Key(dll, name);
    auto& slots = Slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].dll == dll && slots[i].name == name) return g_thunk_base + kSlot * uint32_t(i);
    }
    auto h = Handlers().find(key);
    slots.push_back({dll, name, h == Handlers().end() ? nullptr : h->second});
    uint32_t addr = g_thunk_base + kSlot * uint32_t(slots.size() - 1);
    if (strcmp(name, "_HUGE") == 0) {
        WF64(addr, HUGE_VAL);
    }
    return addr;
}

bool DispatchImport(Ctx* ctx, uint32_t target) {
    if (!g_thunk_base || target < g_thunk_base || target >= g_thunk_base + kThunkSize) return false;
    uint32_t index = (target - g_thunk_base) / kSlot;
    auto& slots = Slots();
    if (index >= slots.size() || (target & (kSlot - 1))) {
        Fatal("call into the import area at %08x", target);
    }
    const Slot& s = slots[index];
    if (!s.fn) {
        Fatal("unimplemented import %s!%s (called from %08x, args %08x %08x %08x %08x)", s.dll.c_str(),
              s.name.c_str(), R32(ctx->esp), Arg(ctx, 0), Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3));
    }
    if (ImportProfiling() && s.name != "SwitchToFiber") {  // a fiber switch does not return until switched back
        ProfiledCall(s, index, ctx);
    } else {
        s.fn(ctx);
    }
    return true;
}

}  // namespace native
