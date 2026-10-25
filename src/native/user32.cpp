// USER32 for the native host. The game window is a single host window (SDL later); handles are fake values.
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "win.h"

namespace native {

std::string GuestFormat(const char* fmt, uint32_t args);  // msvcr80.cpp: printf with guest varargs

namespace {

constexpr uint32_t kMainWindow = 0x00010010u;
constexpr uint32_t kDesktopWindow = 0x00010004u;
uint32_t g_next_window = 0x00010020u;
int g_width = 1280, g_height = 720;  // client size = render resolution (InitClientSize)

std::mutex g_mutex;
std::map<std::pair<uint32_t, int32_t>, uint32_t> g_window_longs;
std::map<std::string, uint32_t> g_classes;  // class name -> WNDPROC
std::map<uint32_t, uint32_t> g_window_proc;

#define U(name) void u_##name(Ctx* c)

U(GetDesktopWindow) { Return(c, kDesktopWindow); }
U(GetForegroundWindow) { Return(c, kMainWindow); }
U(GetClientRect) {
    uint32_t r = Arg(c, 1);
    W32(r, 0);
    W32(r + 4, 0);
    W32(r + 8, uint32_t(g_width));
    W32(r + 12, uint32_t(g_height));
    Return(c, 1, 2);
}
U(GetWindowRect) {
    uint32_t r = Arg(c, 1);
    W32(r, 0);
    W32(r + 4, 0);
    W32(r + 8, uint32_t(g_width));
    W32(r + 12, uint32_t(g_height));
    Return(c, 1, 2);
}
U(GetWindowLongA) {
    std::lock_guard<std::mutex> lock(g_mutex);
    int32_t index = int32_t(Arg(c, 1));
    uint32_t v = g_window_longs[{Arg(c, 0), index}];
    if (index == -4 && !v) v = g_window_proc[Arg(c, 0)];  // GWL_WNDPROC
    Return(c, v, 2);
}
U(SetWindowLongA) {
    std::lock_guard<std::mutex> lock(g_mutex);
    int32_t index = int32_t(Arg(c, 1));
    auto& slot = g_window_longs[{Arg(c, 0), index}];
    uint32_t old = slot;
    if (index == -4) {
        old = g_window_proc[Arg(c, 0)];
        g_window_proc[Arg(c, 0)] = Arg(c, 2);
    }
    slot = Arg(c, 2);
    Return(c, old, 3);
}
U(MapVirtualKeyExA) {
    // MAPVK_VK_TO_VSC for the common keys is enough for the key binding names.
    Return(c, Arg(c, 0) & 0xff, 3);
}
U(GetKeyboardLayout) { Return(c, 0x04090409u, 1); }
U(RegisterClassExA) {
    uint32_t wc = Arg(c, 0);
    uint32_t name = R32(wc + 40);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string n = name < 0x10000 ? "#" + std::to_string(name) : std::string(Str(name));
    g_classes[n] = R32(wc + 8);
    Return(c, 0xc000u + uint32_t(g_classes.size()), 1);
}
U(UnregisterClassA) { Return(c, 1, 2); }
U(CreateWindowExA) {
    uint32_t cls = Arg(c, 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string n = cls < 0x10000 ? "#" + std::to_string(cls) : std::string(Str(cls));
    uint32_t h = g_next_window;
    g_next_window += 4;
    g_window_proc[h] = g_classes[n];
    Log("CreateWindowExA(%s) -> %08x", n.c_str(), h);
    Return(c, h, 12);
}
U(DestroyWindow) { Return(c, 1, 1); }
U(wvsprintfA) {
    std::string s = GuestFormat(Str(Arg(c, 1)), Arg(c, 2));
    strcpy(Str(Arg(c, 0)), s.c_str());
    Return(c, uint32_t(s.size()), 3);
}
U(wsprintfA) {
    // cdecl variadic
    std::string s = GuestFormat(Str(Arg(c, 1)), c->esp + 12);
    strcpy(Str(Arg(c, 0)), s.c_str());
    Return(c, uint32_t(s.size()));
}
U(MessageBoxA) {
    Log("MessageBox [%s]: %s", Arg(c, 2) ? Str(Arg(c, 2)) : "", Arg(c, 1) ? Str(Arg(c, 1)) : "");
    Return(c, 1, 4);  // IDOK
}
U(CharLowerBuffA) {
    char* s = Str(Arg(c, 0));
    for (uint32_t i = 0; i < Arg(c, 1); ++i) s[i] = char(tolower((unsigned char)s[i]));
    Return(c, Arg(c, 1), 2);
}
U(CallWindowProcA) { Return(c, GuestCall(Arg(c, 0), {Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4)}), 5); }
U(DefWindowProcA) { Return(c, 0, 4); }
U(ShowCursor) { Return(c, Arg(c, 0) ? 0 : 0xffffffffu, 1); }
U(GetSystemMetrics) {
    int32_t i = int32_t(Arg(c, 0));
    uint32_t v = i == 0 ? uint32_t(g_width) : i == 1 ? uint32_t(g_height) : 0;  // SM_CXSCREEN / SM_CYSCREEN
    Return(c, v, 1);
}

#undef U

#define E(name) {"user32.dll", #name, u_##name}
const ImportDef kUser32[] = {
    E(GetDesktopWindow), E(GetForegroundWindow), E(GetClientRect), E(GetWindowRect), E(GetWindowLongA),
    E(SetWindowLongA), E(MapVirtualKeyExA), E(GetKeyboardLayout), E(RegisterClassExA), E(UnregisterClassA),
    E(CreateWindowExA), E(DestroyWindow), E(wvsprintfA), E(wsprintfA), E(MessageBoxA), E(CharLowerBuffA),
    E(CallWindowProcA), E(DefWindowProcA), E(ShowCursor), E(GetSystemMetrics),
};
#undef E
ImportRegistrar g_user32(kUser32);

}  // namespace

uint32_t HostWindow() { return kMainWindow; }

// The engine renders at the size of the window's client area. Setting render_height (default 720 on Android, 0 = the
// real window size elsewhere) makes it render smaller, keeping the window's aspect ratio; DXVK scales the frame to
// the window when presenting.
void InitClientSize(int window_w, int window_h) {
#ifdef __ANDROID__
    int rh = SettingInt("render_height", 720);
#else
    int rh = SettingInt("render_height", 0);
#endif
    if (rh > 0 && window_h > 0) {
        g_height = rh;
        g_width = (rh * window_w / window_h + 4) & ~7;
    } else {
        g_width = window_w;
        g_height = window_h;
    }
    Log("render size %dx%d (window %dx%d)", g_width, g_height, window_w, window_h);
}

void SetClientSize(int w, int h) {
    g_width = w;
    g_height = h;
}

void ClientSize(int* w, int* h) {
    *w = g_width;
    *h = g_height;
}

}  // namespace native
