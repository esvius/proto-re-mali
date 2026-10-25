// D3D9 for the native host: the engine's COM calls are forwarded to DXVK (D3D9 on Vulkan, built natively).
//
// Guest objects are 16-byte blocks in guest memory: [0] guest vtable (thunk addresses), [4] index of the host
// object. Method handlers are generated from d3d9.h (tools/gen_d3d9_bridge.py -> d3d9_gen.inc); this file holds the
// object table and the methods whose arguments need conversion: structures with window handles (present
// parameters) and Lock/Unlock, where the host's mapped pointer lies outside guest memory and a guest shadow buffer
// is copied in and out.
#include <SDL2/SDL.h>
#include <d3d9.h>

#include <unistd.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "win.h"

namespace native {

SDL_Window* g_sdl_window = nullptr;
uint32_t HostWindow();
void ClientSize(int* w, int* h);     // user32.cpp
void SetClientSize(int w, int h);

namespace {

struct MethodDef {
    const char* name;
    ImportFn fn;
};
struct InterfaceDef {
    const char* name;
    const MethodDef* methods;
    size_t count;
};

std::recursive_mutex g_mutex;
// index -> host object, read without a lock by every bridged call: fixed chunks that never move, appended to
// under g_mutex.
class HostTable {
public:
    void* Get(uint32_t i) const {
        return chunks_[i >> kBits].load(std::memory_order_acquire)[i & kMask].load(std::memory_order_acquire);
    }
    void Set(uint32_t i, void* p) {
        chunks_[i >> kBits].load(std::memory_order_relaxed)[i & kMask].store(p, std::memory_order_release);
    }
    uint32_t Append(void* p) {
        uint32_t i = size_;
        if ((i >> kBits) >= kChunks) Fatal("d3d9: too many objects");
        if (!chunks_[i >> kBits].load(std::memory_order_relaxed)) {
            chunks_[i >> kBits].store(new std::atomic<void*>[kMask + 1](), std::memory_order_release);
        }
        Set(i, p);
        ++size_;
        return i;
    }

private:
    static constexpr uint32_t kBits = 14, kMask = (1u << kBits) - 1, kChunks = 4096;
    std::atomic<std::atomic<void*>*> chunks_[kChunks] = {};
    uint32_t size_ = 0;
};
HostTable g_hosts;
// host object -> guest object. Child surfaces die with their texture without a Release through the bridge, so an
// entry can be stale: a new object at the same address with another interface must not reuse it.
std::unordered_map<void*, uint32_t> g_guest_of;
std::map<std::string, uint32_t> g_vtables;       // interface name -> guest vtable

template <typename T>
T* Host(uint32_t guest) {
    return static_cast<T*>(g_hosts.Get(R32(guest + 4)));
}

template <typename T>
T* Unwrap(uint32_t guest) {
    return guest ? Host<T>(guest) : nullptr;
}

uint32_t WrapAs(void* host, const char* iface) {
    if (!host) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto vt = g_vtables.find(iface);
    if (vt == g_vtables.end()) Fatal("d3d9: no vtable for %s", iface);
    auto it = g_guest_of.find(host);
    if (it != g_guest_of.end() && R32(it->second) == vt->second) return it->second;
    uint32_t g = HeapAlloc(16);
    W32(g, vt->second);
    W32(g + 4, g_hosts.Append(host));
    g_guest_of[host] = g;
    return g;
}

void Forget(uint32_t guest) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    uint32_t index = R32(guest + 4);
    auto it = g_guest_of.find(g_hosts.Get(index));
    if (it != g_guest_of.end() && it->second == guest) g_guest_of.erase(it);
    g_hosts.Set(index, nullptr);
    W32(guest, 0);
    // The guest block is not reused: a stale guest pointer then faults on a null vtable instead of aliasing.
}

uint32_t Wrap(IDirect3D9* p) { return WrapAs(p, "IDirect3D9"); }
uint32_t Wrap(IDirect3DDevice9* p) { return WrapAs(p, "IDirect3DDevice9"); }
uint32_t Wrap(IDirect3DSwapChain9* p) { return WrapAs(p, "IDirect3DSwapChain9"); }
uint32_t Wrap(IDirect3DSurface9* p) { return WrapAs(p, "IDirect3DSurface9"); }
uint32_t Wrap(IDirect3DVolume9* p) { return WrapAs(p, "IDirect3DVolume9"); }
uint32_t Wrap(IDirect3DVertexBuffer9* p) { return WrapAs(p, "IDirect3DVertexBuffer9"); }
uint32_t Wrap(IDirect3DIndexBuffer9* p) { return WrapAs(p, "IDirect3DIndexBuffer9"); }
uint32_t Wrap(IDirect3DTexture9* p) { return WrapAs(p, "IDirect3DTexture9"); }
uint32_t Wrap(IDirect3DCubeTexture9* p) { return WrapAs(p, "IDirect3DCubeTexture9"); }
uint32_t Wrap(IDirect3DVolumeTexture9* p) { return WrapAs(p, "IDirect3DVolumeTexture9"); }
uint32_t Wrap(IDirect3DVertexDeclaration9* p) { return WrapAs(p, "IDirect3DVertexDeclaration9"); }
uint32_t Wrap(IDirect3DVertexShader9* p) { return WrapAs(p, "IDirect3DVertexShader9"); }
uint32_t Wrap(IDirect3DPixelShader9* p) { return WrapAs(p, "IDirect3DPixelShader9"); }
uint32_t Wrap(IDirect3DStateBlock9* p) { return WrapAs(p, "IDirect3DStateBlock9"); }
uint32_t Wrap(IDirect3DQuery9* p) { return WrapAs(p, "IDirect3DQuery9"); }
uint32_t Wrap(IDirect3DBaseTexture9* p) {
    if (!p) return 0;
    switch (p->GetType()) {
        case D3DRTYPE_CUBETEXTURE: return WrapAs(p, "IDirect3DCubeTexture9");
        case D3DRTYPE_VOLUMETEXTURE: return WrapAs(p, "IDirect3DVolumeTexture9");
        default: return WrapAs(p, "IDirect3DTexture9");
    }
}
uint32_t Wrap(IDirect3DResource9* p) {
    if (!p) return 0;
    switch (p->GetType()) {
        case D3DRTYPE_SURFACE: return WrapAs(p, "IDirect3DSurface9");
        case D3DRTYPE_VOLUME: return WrapAs(p, "IDirect3DVolume9");
        case D3DRTYPE_VERTEXBUFFER: return WrapAs(p, "IDirect3DVertexBuffer9");
        case D3DRTYPE_INDEXBUFFER: return WrapAs(p, "IDirect3DIndexBuffer9");
        default: return Wrap(static_cast<IDirect3DBaseTexture9*>(p));
    }
}

HWND HostHwnd(uint32_t guest_hwnd) { return guest_hwnd ? reinterpret_cast<HWND>(g_sdl_window) : nullptr; }

void ReturnFStd(Ctx* c, float v, int args) {
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
    c->ret_target = Pop32(c);
    c->esp += 4 * args;
}

void ForgetResource(void* host);  // lock shadows and texture adaptation state of a released object

// Vertex and index buffers whose last reference the engine dropped live on for a few frames: the engine sometimes
// still locks a buffer right after releasing it, which on Windows works (the object outlives its last public
// reference while the device uses it) but would hit the bridge's null vtable here. The bridge keeps the engine's
// last reference and drops it at a later Present (or before Reset, which needs default-pool buffers gone).
struct Grave {
    uint32_t guest;
    IUnknown* host;
    uint32_t frame;
};
std::vector<Grave> g_graves;
uint32_t g_present_count = 0;
constexpr uint32_t kGraveFrames = 3;

ULONG RefCount(IUnknown* p) {
    p->AddRef();
    return p->Release();
}

void DrainGraves(bool all) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    size_t kept = 0;
    for (const Grave& gr : g_graves) {
        if (!all && g_present_count - gr.frame < kGraveFrames) {
            g_graves[kept++] = gr;
            continue;
        }
        if (RefCount(gr.host) == 1) {
            ForgetResource(gr.host);
            Forget(gr.guest);
        }
        gr.host->Release();  // revived by the engine meanwhile: its own references keep it
    }
    g_graves.resize(kept);
}

template <typename T>
void ReleaseImpl(Ctx* c) {
    uint32_t g = Arg(c, 0);
    T* host = Host<T>(g);
    if constexpr (std::is_same_v<T, IDirect3DVertexBuffer9> || std::is_same_v<T, IDirect3DIndexBuffer9>) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        for (const Grave& gr : g_graves) {
            if (gr.host == host && RefCount(host) == 1) {  // released again while already pending
                Return(c, 0, 1);
                return;
            }
        }
        if (RefCount(host) == 1) {
            g_graves.push_back({g, host, g_present_count});
            Return(c, 0, 1);
            return;
        }
    }
    ULONG r = host->Release();
    if (r == 0) {
        ForgetResource(host);
        Forget(g);
    }
    Return(c, r, 1);
}

template <typename T>
void QueryInterfaceImpl(Ctx* c) {
    uint32_t g = Arg(c, 0), out = Arg(c, 2);
    const GUID& riid = *Ptr<GUID>(Arg(c, 1));
    if (riid == __uuidof(IUnknown) || riid == __uuidof(T)) {
        Host<T>(g)->AddRef();
        W32(out, g);
        Return(c, S_OK, 3);
        return;
    }
    Log("d3d9: QueryInterface for an unsupported interface");
    W32(out, 0);
    Return(c, E_NOINTERFACE, 3);
}

// ---- present parameters: 56 bytes in guest memory, hDeviceWindow at +28 ----
D3DPRESENT_PARAMETERS ToHost(uint32_t g) {
    D3DPRESENT_PARAMETERS p = {};
    p.BackBufferWidth = R32(g + 0);
    p.BackBufferHeight = R32(g + 4);
    p.BackBufferFormat = D3DFORMAT(R32(g + 8));
    p.BackBufferCount = R32(g + 12);
    p.MultiSampleType = D3DMULTISAMPLE_TYPE(R32(g + 16));
    p.MultiSampleQuality = R32(g + 20);
    p.SwapEffect = D3DSWAPEFFECT(R32(g + 24));
    p.hDeviceWindow = HostHwnd(R32(g + 28) ? R32(g + 28) : HostWindow());
    p.Windowed = R32(g + 32);
    p.EnableAutoDepthStencil = R32(g + 36);
    p.AutoDepthStencilFormat = D3DFORMAT(R32(g + 40));
    p.Flags = R32(g + 44);
    p.FullScreen_RefreshRateInHz = R32(g + 48);
    p.PresentationInterval = R32(g + 52);
    // Android: the SDL window already covers the screen and exclusive fullscreen has no meaning there.
#ifdef __ANDROID__
    const bool windowed = true;
#else
    const bool windowed = getenv("PROTORE_WINDOWED") != nullptr;
#endif
    if (windowed) {
        p.Windowed = TRUE;
        p.FullScreen_RefreshRateInHz = 0;
    }
    return p;
}

void ToGuest(const D3DPRESENT_PARAMETERS& p, uint32_t g) {
    W32(g + 0, p.BackBufferWidth);
    W32(g + 4, p.BackBufferHeight);
    W32(g + 8, p.BackBufferFormat);
    W32(g + 12, p.BackBufferCount);
    W32(g + 16, p.MultiSampleType);
    W32(g + 20, p.MultiSampleQuality);
    W32(g + 24, p.SwapEffect);
    W32(g + 28, HostWindow());
    W32(g + 36, p.EnableAutoDepthStencil);
    W32(g + 40, p.AutoDepthStencilFormat);
    W32(g + 44, p.Flags);
    W32(g + 52, p.PresentationInterval);
}

// ---- lock shadows ----
struct Shadow {
    uint32_t guest = 0, capacity = 0, size = 0;
    void* host = nullptr;
    bool write_back = false;
};
std::map<std::pair<void*, uint32_t>, Shadow> g_shadows;  // (object, subresource) -> shadow

bool IsBlockCompressed(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5 ||
           f == D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '1')) || f == D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '2'));
}

// Maps the host's locked memory into a guest buffer: copies it in unless the lock discards or never overwrites
// (the guest then only writes), and remembers to copy it back on unlock unless read-only.
uint32_t ShadowLock(void* object, uint32_t sub, void* host, uint32_t size, DWORD flags) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Shadow& s = g_shadows[{object, sub}];
    if (s.capacity < size) {
        if (s.guest) HeapFree(s.guest);
        s.guest = HeapAlloc(size);
        s.capacity = size;
    }
    s.size = size;
    s.host = host;
    s.write_back = !(flags & D3DLOCK_READONLY);
    if (!(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) memcpy(Ptr<void>(s.guest), host, size);
    return s.guest;
}

void ShadowUnlock(void* object, uint32_t sub) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_shadows.find({object, sub});
    if (it == g_shadows.end() || !it->second.host) return;
    Shadow& s = it->second;
    if (s.write_back) memcpy(s.host, Ptr<void>(s.guest), s.size);
    s.host = nullptr;
}

void ForgetShadows(void* object) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto it = g_shadows.lower_bound({object, 0}); it != g_shadows.end() && it->first.first == object;) {
        if (it->second.guest) HeapFree(it->second.guest);
        it = g_shadows.erase(it);
    }
}

uint32_t RectRows(const D3DSURFACE_DESC& d, const RECT* r) {
    uint32_t h = r ? uint32_t(r->bottom - r->top) : d.Height;
    return IsBlockCompressed(d.Format) ? (h + 3) / 4 : h;
}

// LockRect common part: D3DLOCKED_RECT in guest memory is {INT Pitch; void* pBits} (8 bytes).
void LockRectCommon(Ctx* c, void* object, uint32_t sub, HRESULT hr, const D3DLOCKED_RECT& lr,
                    const D3DSURFACE_DESC& desc, const RECT* rect, DWORD flags, uint32_t out, int args) {
    if (SUCCEEDED(hr)) {
        uint32_t size = uint32_t(lr.Pitch) * RectRows(desc, rect);
        W32(out, uint32_t(lr.Pitch));
        W32(out + 4, ShadowLock(object, sub, lr.pBits, size, flags));
    }
    Return(c, uint32_t(hr), args);
}

// ---- texture adaptation for mobile GPUs ----
// Invisible to the engine, which keeps seeing the textures it asked for:
//   - mip skipping (setting texture_skip_mips): a large mipmapped texture is created `skip` levels smaller; the
//     guest's top levels go to a scratch buffer and are dropped. Halves texture resolution per level, quarters
//     memory and bandwidth.
//   - DXT decoding (setting decode_dxt, automatic when the GPU has no BC formats, e.g. Mali): DXT1/3/5 textures are
//     created as A8R8G8B8 and decoded on unlock.
struct TexAdapt {
    D3DFORMAT format;   // what the guest asked for
    UINT width, height;
    UINT skip;          // guest level L is host level L - skip; levels below skip are dropped
    bool decode;        // host texture is decode_format, guest writes DXT blocks
    D3DFORMAT decode_format;  // A8R8G8B8, or A1R5G5B5 for DXT1 (half the memory, keeps its 1-bit alpha)
};
std::unordered_map<void*, TexAdapt> g_adapt;  // host texture -> adaptation, only for adapted textures
int g_skip_mips = -1;                          // resolved on first texture
UINT g_skip_min_size = 512;                    // only textures at least this large (larger side) lose levels
bool g_decode_dxt = false;
bool g_decode_16bit = false;  // decode_16bit: DXT1 -> A1R5G5B5 (Android default when the GPU samples it)

bool IsDxt(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

// Bytes per 4x4 block (DXT) or per pixel (plain formats the adaptation handles); 0 = not adaptable.
UINT BlockBytes(D3DFORMAT f) {
    switch (f) {
        case D3DFMT_DXT1: return 8;
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 16;
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return 4;
        default: return 0;
    }
}

// Pitch and row count of a guest level (or of `rect` within it) in the guest's format.
void GuestLevelLayout(D3DFORMAT f, UINT w, UINT h, const RECT* rect, UINT* pitch, UINT* rows) {
    UINT rh = rect ? UINT(rect->bottom - rect->top) : h;
    if (IsDxt(f)) {
        *pitch = ((w + 3) / 4) * BlockBytes(f);
        *rows = (rh + 3) / 4;
    } else {
        *pitch = w * BlockBytes(f);
        *rows = rh;
    }
}

const TexAdapt* AdaptOf(void* host) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_adapt.find(host);
    return it == g_adapt.end() ? nullptr : &it->second;
}

// Decides how a texture of the guest's description is created on the host.
TexAdapt PlanTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format) {
    if (g_skip_mips < 0) {
#ifdef __ANDROID__
        g_skip_mips = SettingInt("texture_skip_mips", 1);
#else
        g_skip_mips = SettingInt("texture_skip_mips", 0);
#endif
        g_skip_min_size = UINT(SettingInt("texture_skip_min_size", 512));
        Log("textures: skip %d mip level(s) of textures >= %u, decode DXT %s", g_skip_mips, g_skip_min_size,
            g_decode_dxt ? "on" : "off");
    }
    TexAdapt a{format, w, h, 0, false, D3DFMT_A8R8G8B8};
    bool plain = !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC));
    if (plain && BlockBytes(format) && levels != 1 && (w > h ? w : h) >= g_skip_min_size) {
        UINT skip = UINT(g_skip_mips);
        // Keep at least one level and 4x4 blocks on the host.
        while (skip && ((levels && skip >= levels) || (w >> skip) < 4 || (h >> skip) < 4)) --skip;
        a.skip = skip;
    }
    a.decode = plain && g_decode_dxt && IsDxt(format);
    if (a.decode && g_decode_16bit && format == D3DFMT_DXT1) a.decode_format = D3DFMT_A1R5G5B5;
    // Statistics: GPU memory of the top level as the guest asked for it and as created.
    static uint64_t count = 0, reduced = 0, decoded = 0, guest_bytes = 0, host_bytes = 0;
    UINT bb = BlockBytes(format);
    if (bb) {
        uint64_t px = uint64_t(w) * h;
        uint64_t g = IsDxt(format) ? px * bb / 16 : px * bb;
        uint64_t hpx = px >> (2 * a.skip);
        guest_bytes += g;
        host_bytes += a.decode ? hpx * (a.decode_format == D3DFMT_A1R5G5B5 ? 2 : 4) : g >> (2 * a.skip);
    }
    ++count;
    reduced += a.skip != 0;
    decoded += a.decode;
    if ((count & 1023) == 0) {
        Log("textures: %llu created, %llu reduced, %llu decoded; top levels %llu MiB -> %llu MiB",
            (unsigned long long)count, (unsigned long long)reduced, (unsigned long long)decoded,
            (unsigned long long)(guest_bytes >> 20), (unsigned long long)(host_bytes >> 20));
    }
    return a;
}

// ---- DXT decoding into A8R8G8B8 (memory order B, G, R, A) ----
void Rgb565(uint16_t c, uint8_t out[4]) {
    out[2] = uint8_t(((c >> 11) & 31) * 255 / 31);
    out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    out[0] = uint8_t((c & 31) * 255 / 31);
    out[3] = 255;
}

// Colour block (DXT1 semantics when `dxt1`, else always four colours) -> 16 BGRA pixels.
void DecodeColorBlock(const uint8_t* b, bool dxt1, uint8_t px[16][4]) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    if (c0 > c1 || !dxt1) {
        for (int k = 0; k < 4; ++k) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
    } else {
        for (int k = 0; k < 4; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;  // transparent black
    }
    uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; ++i) memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}

void DecodeBlock(D3DFORMAT f, const uint8_t* b, uint8_t px[16][4]) {
    if (f == D3DFMT_DXT1) {
        DecodeColorBlock(b, true, px);
    } else if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3) {
        DecodeColorBlock(b + 8, false, px);
        for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
    } else {  // DXT4/5: interpolated alpha
        DecodeColorBlock(b + 8, false, px);
        uint8_t a[8] = {b[0], b[1]};
        if (a[0] > a[1]) {
            for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1] + 3) / 7);
        } else {
            for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1] + 2) / 5);
            a[6] = 0;
            a[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
    }
}

// Guest DXT data (`rows` block rows of `pitch` bytes) -> host BGRA pixels of a w x h region.
void DecodeDxt(D3DFORMAT f, const uint8_t* src, UINT pitch, UINT w, UINT h, uint8_t* dst, INT dst_pitch,
               D3DFORMAT dst_format) {
    const bool packed16 = dst_format == D3DFMT_A1R5G5B5;
    UINT bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = BlockBytes(f);
    uint8_t px[16][4];
    for (UINT by = 0; by < bh; ++by) {
        for (UINT bx = 0; bx < bw; ++bx) {
            DecodeBlock(f, src + by * pitch + bx * bytes, px);
            for (UINT y = 0; y < 4 && by * 4 + y < h; ++y) {
                for (UINT x = 0; x < 4 && bx * 4 + x < w; ++x) {
                    const uint8_t* p = px[y * 4 + x];
                    if (packed16) {  // B, G, R, A -> A1R5G5B5
                        uint16_t v = uint16_t((p[3] >= 128 ? 0x8000 : 0) | (p[2] >> 3) << 10 | (p[1] >> 3) << 5 |
                                              (p[0] >> 3));
                        memcpy(dst + INT(by * 4 + y) * dst_pitch + (bx * 4 + x) * 2, &v, 2);
                    } else {
                        memcpy(dst + INT(by * 4 + y) * dst_pitch + (bx * 4 + x) * 4, p, 4);
                    }
                }
            }
        }
    }
}

// ---- locks of adapted textures ----
struct AdaptedLock {
    uint32_t guest = 0, capacity = 0;  // guest buffer in the guest format
    UINT pitch = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN, decode_format = D3DFMT_A8R8G8B8;
    UINT width = 0, height = 0;         // region being written
    uint8_t* host = nullptr;            // locked host memory (decode target), null for dropped levels
    INT host_pitch = 0;
};
std::map<std::pair<void*, uint32_t>, AdaptedLock> g_adapted_locks;

// Locks guest level `level` of an adapted texture whose host level is locked through `lock_host`
// (HRESULT(UINT host_level, D3DLOCKED_RECT*)); writes D3DLOCKED_RECT to the guest.
template <typename LockHost>
HRESULT AdaptedLockRect(void* tex, uint32_t sub, const TexAdapt& a, UINT level, const RECT* rect, DWORD flags,
                        uint32_t out, LockHost lock_host) {
    UINT w = a.width >> level ? a.width >> level : 1, h = a.height >> level ? a.height >> level : 1;
    UINT pitch, rows;
    GuestLevelLayout(a.format, w, h, rect, &pitch, &rows);
    D3DLOCKED_RECT lr{};
    bool dropped = level < a.skip;
    if (!dropped) {
        HRESULT hr = lock_host(level - a.skip, &lr);
        if (FAILED(hr)) return hr;
        if (!a.decode) {  // same format and size on the host: plain shadow
            W32(out, uint32_t(lr.Pitch));
            W32(out + 4, ShadowLock(tex, sub, lr.pBits, uint32_t(lr.Pitch) * rows, flags));
            return D3D_OK;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    AdaptedLock& l = g_adapted_locks[{tex, sub}];
    uint32_t size = pitch * rows;
    if (l.capacity < size) {
        if (l.guest) HeapFree(l.guest);
        l.guest = HeapAlloc(size);
        l.capacity = size;
    }
    l.pitch = pitch;
    l.format = a.format;
    l.decode_format = a.decode_format;
    l.width = rect ? UINT(rect->right - rect->left) : w;
    l.height = rect ? UINT(rect->bottom - rect->top) : h;
    l.host = dropped ? nullptr : static_cast<uint8_t*>(lr.pBits);
    l.host_pitch = lr.Pitch;
    W32(out, pitch);
    W32(out + 4, l.guest);
    return D3D_OK;
}

// Returns true when the lock was a decoded or dropped one (the host level, if any, must still be unlocked).
bool AdaptedUnlock(void* tex, uint32_t sub, bool* host_locked) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_adapted_locks.find({tex, sub});
    if (it == g_adapted_locks.end() || !it->second.guest) return false;
    AdaptedLock& l = it->second;
    *host_locked = l.host != nullptr;
    if (l.host) {
        DecodeDxt(l.format, Ptr<uint8_t>(l.guest), l.pitch, l.width, l.height, l.host, l.host_pitch, l.decode_format);
        l.host = nullptr;
    }
    return true;
}

void ForgetResource(void* host) {
    ForgetShadows(host);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_adapt.erase(host);
    for (auto it = g_adapted_locks.lower_bound({host, 0}); it != g_adapted_locks.end() && it->first.first == host;) {
        if (it->second.guest) HeapFree(it->second.guest);
        it = g_adapted_locks.erase(it);
    }
}

// Guest view of a level description: guest size and format.
void AdaptDesc(const TexAdapt& a, UINT level, D3DSURFACE_DESC* d) {
    d->Format = a.format;
    d->Width = a.width >> level ? a.width >> level : 1;
    d->Height = a.height >> level ? a.height >> level : 1;
}


// ---- display modes ----
// With override_modes (default on Android) the engine sees modes of the render resolution instead of the screen's:
// the render size (render_height) first, then 540p/720p/900p/1080p at the screen's aspect ratio. Its default
// fullscreen choice, the back buffer and the window client area then agree, and touch coordinates map 1:1.
bool OverrideModes() {
#ifdef __ANDROID__
    static const bool on = SettingInt("override_modes", 1) != 0;
#else
    static const bool on = SettingInt("override_modes", 0) != 0;
#endif
    return on;
}

std::vector<D3DDISPLAYMODE> Modes() {
    static std::vector<D3DDISPLAYMODE> modes;
    if (modes.empty()) {
        int cw, ch;
        ClientSize(&cw, &ch);
        modes.push_back({UINT(cw), UINT(ch), 60, D3DFMT_X8R8G8B8});
        for (UINT h : {540u, 720u, 900u, 1080u}) {
            UINT w = (UINT(uint64_t(h) * UINT(cw) / UINT(ch)) + 4) & ~7u;
            if (h != UINT(ch)) modes.push_back({w, h, 60, D3DFMT_X8R8G8B8});
        }
    }
    return modes;
}

void WriteMode(uint32_t out, const D3DDISPLAYMODE& m) {
    W32(out, m.Width);
    W32(out + 4, m.Height);
    W32(out + 8, m.RefreshRate);
    W32(out + 12, uint32_t(m.Format));
}

// ---- Direct3DCreate9 ----
void x_Direct3DCreate9(Ctx* c) {
    IDirect3D9* d3d = Direct3DCreate9(Arg(c, 0));
    Log("Direct3DCreate9(%u) -> %p", Arg(c, 0), static_cast<void*>(d3d));
    // GPUs without BC formats (Mali, PowerVR) get DXT textures decoded on the CPU; decode_dxt=1/0 forces it.
    if (d3d) {
        bool bc = SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
                                                   D3DFMT_DXT5));
        g_decode_dxt = SettingInt("decode_dxt", bc ? 0 : 1) != 0;
        bool a1r5 = SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
                                                     D3DFMT_A1R5G5B5));
#ifdef __ANDROID__
        g_decode_16bit = a1r5 && SettingInt("decode_16bit", 1) != 0;
#else
        g_decode_16bit = a1r5 && SettingInt("decode_16bit", 0) != 0;
#endif
        Log("GPU BC (DXT) textures: %s, decoding %s%s", bc ? "supported" : "not supported", g_decode_dxt ? "on" : "off",
            g_decode_dxt && g_decode_16bit ? " (DXT1 as 16-bit)" : "");
    }
    Return(c, Wrap(d3d), 1);
}

}  // namespace

// ---------------- manual methods ----------------
#define M(iface, method) void Manual_##iface##_##method(Ctx* c)

M(IDirect3D9, GetAdapterMonitor) { Return(c, 1, 2); }
M(IDirect3D9, GetAdapterDisplayMode) {
    // (this, Adapter, pMode)
    if (!OverrideModes()) {
        Return(c, uint32_t(Host<IDirect3D9>(Arg(c, 0))->GetAdapterDisplayMode(Arg(c, 1), Ptr<D3DDISPLAYMODE>(Arg(c, 2)))),
               3);
        return;
    }
    WriteMode(Arg(c, 2), Modes()[0]);
    Return(c, D3D_OK, 3);
}
M(IDirect3D9, GetAdapterModeCount) {
    // (this, Adapter, Format)
    if (!OverrideModes()) {
        Return(c, Host<IDirect3D9>(Arg(c, 0))->GetAdapterModeCount(Arg(c, 1), D3DFORMAT(Arg(c, 2))), 3);
        return;
    }
    D3DFORMAT f = D3DFORMAT(Arg(c, 2));
    Return(c, f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8 ? uint32_t(Modes().size()) : 0u, 3);
}
M(IDirect3D9, EnumAdapterModes) {
    // (this, Adapter, Format, Mode, pMode)
    if (!OverrideModes()) {
        Return(c, uint32_t(Host<IDirect3D9>(Arg(c, 0))->EnumAdapterModes(Arg(c, 1), D3DFORMAT(Arg(c, 2)), Arg(c, 3),
                                                                        Ptr<D3DDISPLAYMODE>(Arg(c, 4)))),
               5);
        return;
    }
    auto modes = Modes();
    if (Arg(c, 3) >= modes.size()) {
        Return(c, uint32_t(D3DERR_INVALIDCALL), 5);
        return;
    }
    D3DDISPLAYMODE m = modes[Arg(c, 3)];
    m.Format = D3DFORMAT(Arg(c, 2));
    WriteMode(Arg(c, 4), m);
    Return(c, D3D_OK, 5);
}
M(IDirect3DDevice9, GetDisplayMode) {
    // (this, iSwapChain, pMode)
    if (!OverrideModes()) {
        Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->GetDisplayMode(Arg(c, 1),
                                                                            Ptr<D3DDISPLAYMODE>(Arg(c, 2)))),
               3);
        return;
    }
    WriteMode(Arg(c, 2), Modes()[0]);
    Return(c, D3D_OK, 3);
}

bool WaitForWindow(int seconds);  // below, with the surface state

M(IDirect3D9, CreateDevice) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 5));
    IDirect3DDevice9* dev = nullptr;
    // The Vulkan surface needs the window on screen: if the player switched to another app during startup, the
    // device would fail with a lost surface. Wait for the window, and retry a failure that happened while it was gone.
    HRESULT hr = D3DERR_INVALIDCALL;
    for (int attempt = 0; attempt < 5; ++attempt) {
        WaitForWindow(600);
        D3DPRESENT_PARAMETERS try_pp = pp;
        hr = Host<IDirect3D9>(Arg(c, 0))->CreateDevice(Arg(c, 1), D3DDEVTYPE(Arg(c, 2)), HostHwnd(Arg(c, 3)),
                                                     Arg(c, 4), &try_pp, &dev);
        if (SUCCEEDED(hr)) {
            pp = try_pp;
            break;
        }
        if (!WaitForWindow(0)) {
            Log("CreateDevice -> %08x while the window was away, retrying", uint32_t(hr));
            continue;
        }
        break;  // failed with the window on screen: a real driver problem
    }
    Log("CreateDevice %ux%u fmt %u windowed %u msaa %u/%u interval %u -> %08x", pp.BackBufferWidth,
        pp.BackBufferHeight, pp.BackBufferFormat, pp.Windowed, pp.MultiSampleType, pp.MultiSampleQuality,
        pp.PresentationInterval, uint32_t(hr));
    // The engine does not check: without a device it crashes on the next call. The DXVK log says why it failed.
    if (FAILED(hr)) {
        StartupFailure("The graphics driver could not create the D3D9 device (DXVK on Vulkan). "
                       "See app_process64_d3d9.log for the reason.");
    }
    if (OverrideModes()) SetClientSize(int(pp.BackBufferWidth), int(pp.BackBufferHeight));
    ToGuest(pp, Arg(c, 5));
    W32(Arg(c, 6), Wrap(dev));
    Return(c, uint32_t(hr), 7);
}
M(IDirect3DDevice9, GetCreationParameters) {
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->GetCreationParameters(&p);
    uint32_t g = Arg(c, 1);
    W32(g, p.AdapterOrdinal);
    W32(g + 4, p.DeviceType);
    W32(g + 8, HostWindow());
    W32(g + 12, p.BehaviorFlags);
    Return(c, uint32_t(hr), 2);
}
M(IDirect3DDevice9, SetCursorProperties) {
    Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->SetCursorProperties(
                  Arg(c, 1), Arg(c, 2), Unwrap<IDirect3DSurface9>(Arg(c, 3)))),
           4);
}
M(IDirect3DDevice9, CreateAdditionalSwapChain) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 1));
    IDirect3DSwapChain9* sc = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateAdditionalSwapChain(&pp, &sc);
    ToGuest(pp, Arg(c, 1));
    W32(Arg(c, 2), SUCCEEDED(hr) ? Wrap(sc) : 0);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DDevice9, Reset) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 1));
    DrainGraves(true);
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->Reset(&pp);
    Log("Reset %ux%u msaa %u/%u interval %u -> %08x", pp.BackBufferWidth, pp.BackBufferHeight, pp.MultiSampleType,
        pp.MultiSampleQuality, pp.PresentationInterval, uint32_t(hr));
    if (SUCCEEDED(hr) && OverrideModes()) SetClientSize(int(pp.BackBufferWidth), int(pp.BackBufferHeight));
    ToGuest(pp, Arg(c, 1));
    Return(c, uint32_t(hr), 2);
}
// Setting shot=<frame>[,<frame>...] (PROTORE_SHOT): the back buffer of those presents is saved as shot_<frame>.ppm in
// the working directory (debugging without a desktop screenshot tool; on Android only the game's own frame).
void MaybeScreenshot(IDirect3DDevice9* dev) {
    static const std::string spec_setting = Setting("shot", "");  // protore.ini `shot = ...` works on Android
    static const char* spec = spec_setting.empty() ? nullptr : spec_setting.c_str();
    static uint32_t frame = 0;
    ++frame;
    // On demand: a file named shot_now in the working directory (adb shell touch ...) asks for one frame.
    bool now = (frame & 15) == 0 && access("shot_now", F_OK) == 0;
    if (now) unlink("shot_now");
    if (!spec && !now) return;
    bool want = now;
    for (const char* p = spec ? spec : ""; *p;) {
        char* end;
        if (strtoul(p, &end, 10) == frame) want = true;
        p = *end ? end + 1 : end;
    }
    if (!want) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    D3DLOCKED_RECT lr;
    if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(bb, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        std::string name = now ? std::string("shot_now.ppm") : "shot_" + std::to_string(frame) + ".ppm";
        if (FILE* f = fopen(name.c_str(), "wb")) {
            fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
            std::vector<uint8_t> row(3 * d.Width);
            for (uint32_t y = 0; y < d.Height; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * size_t(lr.Pitch);
                for (uint32_t x = 0; x < d.Width; ++x) {
                    row[3 * x] = src[4 * x + 2];
                    row[3 * x + 1] = src[4 * x + 1];
                    row[3 * x + 2] = src[4 * x];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
            Log("screenshot %s (%ux%u)", name.c_str(), d.Width, d.Height);
        }
        sys->UnlockRect();
    } else {
        Log("screenshot of frame %u failed (format %d)", frame, int(d.Format));
    }
    if (sys) sys->Release();
    bb->Release();
}

// The window can go away under the render thread (Android destroys the surface when the app leaves the screen):
// ProtoSurface.surfaceDestroyed waits for a present in flight and holds presents back until the surface is back,
// as presenting or recreating the swap chain on a destroyed window crashes in the Vulkan loader. A skipped present
// just drops the frame.
std::mutex g_present_mutex;
bool g_surface_gone = false;  // under g_present_mutex

template <typename F>
HRESULT GuardedPresent(F present) {
    std::unique_lock<std::mutex> lock(g_present_mutex);
    if (g_surface_gone) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return D3D_OK;
    }
    return present();
}

#ifdef __ANDROID__
// Waits up to `seconds` for the game's window (surface) to be on screen; true if it is.
bool WaitForWindow(int seconds) {
    for (int i = 0; i <= seconds * 10; ++i) {
        {
            std::lock_guard<std::mutex> lock(g_present_mutex);
            if (!g_surface_gone) return true;
        }
        if (i < seconds * 10) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

extern "C" JNIEXPORT void JNICALL Java_com_protore_game_ProtoSurface_nativeSurfaceGone(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_present_mutex);
    g_surface_gone = true;
}
extern "C" JNIEXPORT void JNICALL Java_com_protore_game_ProtoSurface_nativeSurfaceReady(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_present_mutex);
    g_surface_gone = false;
}
#endif

M(IDirect3DDevice9, Present) {
    ++g_present_count;
    if (!g_graves.empty()) DrainGraves(false);
    auto* dev = Host<IDirect3DDevice9>(Arg(c, 0));
    MaybeScreenshot(dev);
    HRESULT hr =
        GuardedPresent([&] { return dev->Present(Ptr<RECT>(Arg(c, 1)), Ptr<RECT>(Arg(c, 2)), nullptr, nullptr); });
    Return(c, uint32_t(hr), 5);
}
// Sampler state passes through; the first 200000 calls are tallied once to log the filtering the engine asks for
// (anisotropy and mip filtering are what a phone GPU pays for).
M(IDirect3DDevice9, SetSamplerState) {
    static std::atomic<uint32_t> calls{0};
    static std::atomic<uint32_t> aniso[17], minf[4], mipf[4];
    const DWORD type = Arg(c, 2), value = Arg(c, 3);
    if (uint32_t n = calls.fetch_add(1); n < 200000) {
        if (type == D3DSAMP_MAXANISOTROPY) ++aniso[value > 16 ? 16 : value];
        if (type == D3DSAMP_MINFILTER) ++minf[value & 3];
        if (type == D3DSAMP_MIPFILTER) ++mipf[value & 3];
        if (n == 199999) {
            std::string a;
            for (int i = 0; i <= 16; ++i)
                if (aniso[i]) a += " " + std::to_string(i) + "x:" + std::to_string(aniso[i].load());
            Log("samplers: max anisotropy%s | min filter none/point/linear/aniso %u/%u/%u/%u | mip %u/%u/%u/%u",
                a.c_str(), minf[0].load(), minf[1].load(), minf[2].load(), minf[3].load(), mipf[0].load(),
                mipf[1].load(), mipf[2].load(), mipf[3].load());
        }
    }
    Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->SetSamplerState(Arg(c, 1), D3DSAMPLERSTATETYPE(type),
                                                                          value)), 4);
}
// The engine's worker threads poll this before every buffer lock; DXVK answers D3D_OK for a windowed device but
// takes the device lock first, so the polls queue up behind the render thread's draws and presents.
M(IDirect3DDevice9, TestCooperativeLevel) { Return(c, uint32_t(D3D_OK), 1); }
M(IDirect3DSwapChain9, Present) {
    auto* sc = Host<IDirect3DSwapChain9>(Arg(c, 0));
    HRESULT hr = GuardedPresent([&] { return sc->Present(Ptr<RECT>(Arg(c, 1)), Ptr<RECT>(Arg(c, 2)), nullptr, nullptr,
                                                         Arg(c, 5)); });
    Return(c, uint32_t(hr), 6);
}
M(IDirect3DSwapChain9, GetPresentParameters) {
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr = Host<IDirect3DSwapChain9>(Arg(c, 0))->GetPresentParameters(&pp);
    ToGuest(pp, Arg(c, 1));
    Return(c, uint32_t(hr), 2);
}

// GetContainer(riid, void**): the container is a texture or the device.
template <typename T>
void GetContainerImpl(Ctx* c) {
    const GUID& riid = *Ptr<GUID>(Arg(c, 1));
    void* p = nullptr;
    HRESULT hr = Host<T>(Arg(c, 0))->GetContainer(riid, &p);
    uint32_t g = 0;
    if (SUCCEEDED(hr)) {
        if (riid == __uuidof(IDirect3DTexture9)) g = Wrap(static_cast<IDirect3DTexture9*>(p));
        else if (riid == __uuidof(IDirect3DCubeTexture9)) g = Wrap(static_cast<IDirect3DCubeTexture9*>(p));
        else if (riid == __uuidof(IDirect3DVolumeTexture9)) g = Wrap(static_cast<IDirect3DVolumeTexture9*>(p));
        else if (riid == __uuidof(IDirect3DBaseTexture9)) g = Wrap(static_cast<IDirect3DBaseTexture9*>(p));
        else if (riid == __uuidof(IDirect3DDevice9)) g = Wrap(static_cast<IDirect3DDevice9*>(p));
        else Log("d3d9: GetContainer for an unsupported interface");
    }
    W32(Arg(c, 2), g);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DSurface9, GetContainer) { GetContainerImpl<IDirect3DSurface9>(c); }
M(IDirect3DVolume9, GetContainer) { GetContainerImpl<IDirect3DVolume9>(c); }

M(IDirect3DSurface9, LockRect) {
    auto* s = Host<IDirect3DSurface9>(Arg(c, 0));
    const RECT* rect = Ptr<RECT>(Arg(c, 2));
    D3DLOCKED_RECT lr;
    HRESULT hr = s->LockRect(&lr, rect, Arg(c, 3));
    D3DSURFACE_DESC desc;
    s->GetDesc(&desc);
    LockRectCommon(c, s, 0, hr, lr, desc, rect, Arg(c, 3), Arg(c, 1), 4);
}
M(IDirect3DSurface9, UnlockRect) {
    auto* s = Host<IDirect3DSurface9>(Arg(c, 0));
    ShadowUnlock(s, 0);
    Return(c, uint32_t(s->UnlockRect()), 1);
}
M(IDirect3DSurface9, GetDC) { Return(c, uint32_t(D3DERR_INVALIDCALL), 2); }
M(IDirect3DSurface9, ReleaseDC) { Return(c, uint32_t(D3DERR_INVALIDCALL), 2); }

M(IDirect3DDevice9, CreateTexture) {
    // (this, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
    UINT w = Arg(c, 1), h = Arg(c, 2), levels = Arg(c, 3);
    DWORD usage = Arg(c, 4);
    D3DFORMAT format = D3DFORMAT(Arg(c, 5));
    TexAdapt a = PlanTexture(w, h, levels, usage, format);
    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateTexture(
        w >> a.skip, h >> a.skip, levels ? levels - a.skip : 0, usage, a.decode ? a.decode_format : format,
        D3DPOOL(Arg(c, 6)), &tex, nullptr);
    if (SUCCEEDED(hr) && (a.skip || a.decode)) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_adapt[tex] = a;
    }
    W32(Arg(c, 7), SUCCEEDED(hr) ? Wrap(tex) : 0);
    Return(c, uint32_t(hr), 9);
}
M(IDirect3DDevice9, CreateCubeTexture) {
    // (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle)
    UINT edge = Arg(c, 1), levels = Arg(c, 2);
    DWORD usage = Arg(c, 3);
    D3DFORMAT format = D3DFORMAT(Arg(c, 4));
    TexAdapt a = PlanTexture(edge, edge, levels, usage, format);
    IDirect3DCubeTexture9* tex = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateCubeTexture(
        edge >> a.skip, levels ? levels - a.skip : 0, usage, a.decode ? a.decode_format : format, D3DPOOL(Arg(c, 5)),
        &tex, nullptr);
    if (SUCCEEDED(hr) && (a.skip || a.decode)) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_adapt[tex] = a;
    }
    W32(Arg(c, 6), SUCCEEDED(hr) ? Wrap(tex) : 0);
    Return(c, uint32_t(hr), 8);
}
M(IDirect3D9, CheckDeviceFormat) {
    // (this, Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat): DXT is always available when decoded.
    D3DFORMAT check = D3DFORMAT(Arg(c, 6));
    HRESULT hr = Host<IDirect3D9>(Arg(c, 0))->CheckDeviceFormat(Arg(c, 1), D3DDEVTYPE(Arg(c, 2)),
                                                              D3DFORMAT(Arg(c, 3)), Arg(c, 4),
                                                              D3DRESOURCETYPE(Arg(c, 5)), check);
    if (FAILED(hr) && g_decode_dxt && IsDxt(check) && !(Arg(c, 4) & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
        hr = D3D_OK;
    }
    Return(c, uint32_t(hr), 7);
}
M(IDirect3DTexture9, GetLevelCount) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    Return(c, t->GetLevelCount() + (a ? a->skip : 0), 1);
}
M(IDirect3DCubeTexture9, GetLevelCount) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    Return(c, t->GetLevelCount() + (a ? a->skip : 0), 1);
}
M(IDirect3DTexture9, GetLevelDesc) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    UINT level = Arg(c, 1);
    const TexAdapt* a = AdaptOf(t);
    D3DSURFACE_DESC* d = Ptr<D3DSURFACE_DESC>(Arg(c, 2));
    HRESULT hr = t->GetLevelDesc(a && level >= a->skip ? level - a->skip : a ? 0 : level, d);
    if (SUCCEEDED(hr) && a) AdaptDesc(*a, level, d);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DCubeTexture9, GetLevelDesc) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    UINT level = Arg(c, 1);
    const TexAdapt* a = AdaptOf(t);
    D3DSURFACE_DESC* d = Ptr<D3DSURFACE_DESC>(Arg(c, 2));
    HRESULT hr = t->GetLevelDesc(a && level >= a->skip ? level - a->skip : a ? 0 : level, d);
    if (SUCCEEDED(hr) && a) AdaptDesc(*a, level, d);
    Return(c, uint32_t(hr), 3);
}
// Surfaces of adapted textures: the nearest host level (dropped levels map to the top host level).
M(IDirect3DTexture9, GetSurfaceLevel) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    UINT level = Arg(c, 1);
    IDirect3DSurface9* surf = nullptr;
    HRESULT hr = t->GetSurfaceLevel(a ? (level >= a->skip ? level - a->skip : 0) : level, &surf);
    W32(Arg(c, 2), SUCCEEDED(hr) ? Wrap(surf) : 0);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DCubeTexture9, GetCubeMapSurface) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    UINT level = Arg(c, 2);
    IDirect3DSurface9* surf = nullptr;
    HRESULT hr = t->GetCubeMapSurface(D3DCUBEMAP_FACES(Arg(c, 1)), a ? (level >= a->skip ? level - a->skip : 0) : level,
                                      &surf);
    W32(Arg(c, 3), SUCCEEDED(hr) ? Wrap(surf) : 0);
    Return(c, uint32_t(hr), 4);
}
M(IDirect3DTexture9, LockRect) {
    // (this, Level, pLockedRect, pRect, Flags)
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const RECT* rect = Ptr<RECT>(Arg(c, 3));
    if (const TexAdapt* a = AdaptOf(t)) {
        TexAdapt copy = *a;
        HRESULT hr = AdaptedLockRect(t, level, copy, level, rect, Arg(c, 4), Arg(c, 2),
                                     [&](UINT host_level, D3DLOCKED_RECT* lr) {
                                         return t->LockRect(host_level, lr, rect, Arg(c, 4));
                                     });
        Return(c, uint32_t(hr), 5);
        return;
    }
    D3DLOCKED_RECT lr;
    HRESULT hr = t->LockRect(level, &lr, rect, Arg(c, 4));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockRectCommon(c, t, level, hr, lr, desc, rect, Arg(c, 4), Arg(c, 2), 5);
}
M(IDirect3DTexture9, UnlockRect) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const TexAdapt* a = AdaptOf(t);
    bool host_locked = true;
    if (a && AdaptedUnlock(t, level, &host_locked)) {
        Return(c, host_locked ? uint32_t(t->UnlockRect(level - a->skip)) : 0u, 2);
        return;
    }
    ShadowUnlock(t, level);
    Return(c, uint32_t(t->UnlockRect(a ? level - a->skip : level)), 2);
}
M(IDirect3DCubeTexture9, LockRect) {
    // (this, FaceType, Level, pLockedRect, pRect, Flags)
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    uint32_t face = Arg(c, 1), level = Arg(c, 2);
    const RECT* rect = Ptr<RECT>(Arg(c, 4));
    if (const TexAdapt* a = AdaptOf(t)) {
        TexAdapt copy = *a;
        HRESULT hr = AdaptedLockRect(t, face * 64 + level, copy, level, rect, Arg(c, 5), Arg(c, 3),
                                     [&](UINT host_level, D3DLOCKED_RECT* lr) {
                                         return t->LockRect(D3DCUBEMAP_FACES(face), host_level, lr, rect, Arg(c, 5));
                                     });
        Return(c, uint32_t(hr), 6);
        return;
    }
    D3DLOCKED_RECT lr;
    HRESULT hr = t->LockRect(D3DCUBEMAP_FACES(face), level, &lr, rect, Arg(c, 5));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockRectCommon(c, t, face * 64 + level, hr, lr, desc, rect, Arg(c, 5), Arg(c, 3), 6);
}
M(IDirect3DCubeTexture9, UnlockRect) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    uint32_t face = Arg(c, 1), level = Arg(c, 2);
    const TexAdapt* a = AdaptOf(t);
    bool host_locked = true;
    if (a && AdaptedUnlock(t, face * 64 + level, &host_locked)) {
        Return(c, host_locked ? uint32_t(t->UnlockRect(D3DCUBEMAP_FACES(face), level - a->skip)) : 0u, 3);
        return;
    }
    ShadowUnlock(t, face * 64 + level);
    Return(c, uint32_t(t->UnlockRect(D3DCUBEMAP_FACES(face), a ? level - a->skip : level)), 3);
}

// D3DLOCKED_BOX in guest memory: {INT RowPitch; INT SlicePitch; void* pBits} (12 bytes).
void LockBoxCommon(Ctx* c, void* object, uint32_t sub, HRESULT hr, const D3DLOCKED_BOX& lb, uint32_t depth,
                   DWORD flags, uint32_t out, int args) {
    if (SUCCEEDED(hr)) {
        W32(out, uint32_t(lb.RowPitch));
        W32(out + 4, uint32_t(lb.SlicePitch));
        W32(out + 8, ShadowLock(object, sub, lb.pBits, uint32_t(lb.SlicePitch) * depth, flags));
    }
    Return(c, uint32_t(hr), args);
}
M(IDirect3DVolumeTexture9, LockBox) {
    auto* t = Host<IDirect3DVolumeTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const D3DBOX* box = Ptr<D3DBOX>(Arg(c, 3));
    D3DLOCKED_BOX lb;
    HRESULT hr = t->LockBox(level, &lb, box, Arg(c, 4));
    D3DVOLUME_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockBoxCommon(c, t, level, hr, lb, box ? box->Back - box->Front : desc.Depth, Arg(c, 4), Arg(c, 2), 5);
}
M(IDirect3DVolumeTexture9, UnlockBox) {
    auto* t = Host<IDirect3DVolumeTexture9>(Arg(c, 0));
    ShadowUnlock(t, Arg(c, 1));
    Return(c, uint32_t(t->UnlockBox(Arg(c, 1))), 2);
}
M(IDirect3DVolume9, LockBox) {
    auto* v = Host<IDirect3DVolume9>(Arg(c, 0));
    const D3DBOX* box = Ptr<D3DBOX>(Arg(c, 2));
    D3DLOCKED_BOX lb;
    HRESULT hr = v->LockBox(&lb, box, Arg(c, 3));
    D3DVOLUME_DESC desc;
    v->GetDesc(&desc);
    LockBoxCommon(c, v, 0, hr, lb, box ? box->Back - box->Front : desc.Depth, Arg(c, 3), Arg(c, 1), 4);
}
M(IDirect3DVolume9, UnlockBox) {
    auto* v = Host<IDirect3DVolume9>(Arg(c, 0));
    ShadowUnlock(v, 0);
    Return(c, uint32_t(v->UnlockBox()), 1);
}

template <typename T, typename Desc>
void BufferLock(Ctx* c) {
    auto* b = Host<T>(Arg(c, 0));
    uint32_t offset = Arg(c, 1), size = Arg(c, 2), out = Arg(c, 3);
    DWORD flags = Arg(c, 4);
    void* p = nullptr;
    HRESULT hr = b->Lock(offset, size, &p, flags);
    if (SUCCEEDED(hr)) {
        if (!size) {
            Desc d;
            b->GetDesc(&d);
            size = d.Size - offset;
        }
        W32(out, ShadowLock(b, 0, p, size, flags));
    }
    Return(c, uint32_t(hr), 5);
}
template <typename T>
void BufferUnlock(Ctx* c) {
    auto* b = Host<T>(Arg(c, 0));
    ShadowUnlock(b, 0);
    Return(c, uint32_t(b->Unlock()), 1);
}
M(IDirect3DVertexBuffer9, Lock) { BufferLock<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(c); }
M(IDirect3DVertexBuffer9, Unlock) { BufferUnlock<IDirect3DVertexBuffer9>(c); }
M(IDirect3DIndexBuffer9, Lock) { BufferLock<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(c); }
M(IDirect3DIndexBuffer9, Unlock) { BufferUnlock<IDirect3DIndexBuffer9>(c); }

#undef M

#include "d3d9_gen.inc"

namespace {

// Builds the guest vtables (one thunk per method) and registers Direct3DCreate9.
struct Registrar {
    Registrar() {
        static const ImportDef create[] = {{"d3d9.dll", "Direct3DCreate9", x_Direct3DCreate9}};
        RegisterImports(create, 1);
        for (const InterfaceDef& i : kInterfaces) {
            std::vector<ImportDef> defs;
            for (size_t k = 0; k < i.count; ++k) {
                std::string n = std::string(i.name) + "::" + i.methods[k].name;
                defs.push_back({"d3d9.com", strdup(n.c_str()), i.methods[k].fn});
            }
            RegisterImports(defs.data(), defs.size());
        }
    }
} g_registrar;

}  // namespace

// Called once guest memory exists (the vtables live there).
void InitD3D9Bridge() {
    for (const InterfaceDef& i : kInterfaces) {
        uint32_t vt = HeapAlloc(uint32_t(4 * i.count));
        for (size_t k = 0; k < i.count; ++k) {
            std::string n = std::string(i.name) + "::" + i.methods[k].name;
            W32(vt + uint32_t(4 * k), ImportThunk("d3d9.com", n.c_str()));
        }
        g_vtables[i.name] = vt;
    }
}

}  // namespace native
