// DirectInput 8 (ANSI interfaces) for the native host, on SDL input state.
//
// The engine's core::ControllerSystemDirectInput gets IDirectInput8A from CoCreateInstance, enumerates all attached
// devices and turns each into a controller: EnumObjects lists its buttons and axes, SetDataFormat installs a format
// with one dword per object, and every frame it calls Acquire, Poll and GetDeviceState. The system keyboard and
// mouse must show up here: the game's PhysicalController for keyboard/mouse is only bound once they are announced.
//
// Guest objects are 16-byte blocks: [0] guest vtable (import thunks), [4] index into g_devices (0 = the
// IDirectInput8A object itself).
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "win.h"

namespace native {

std::vector<uint8_t> KeyboardDiks();  // input.cpp
bool DikDown(uint8_t dik);
void TakeMouseMotion(int32_t* dx, int32_t* dy, int32_t* dz);
uint32_t MouseButtonMask();

namespace {

constexpr uint32_t kDI_OK = 0, kDIERR_Unsupported = 0x80004001u, kDIERR_DeviceNotReg = 0x80040154u,
                   kDIERR_ObjectNotFound = 0x80070002u, kE_NoInterface = 0x80004002u, kE_Pointer = 0x80004003u;

// DIDFT_* object type bits.
constexpr uint32_t kRelAxis = 0x01, kAxis = 0x03, kPshButton = 0x04, kButton = 0x0c,
                   kAnyInstance = 0x00ffff00u;
constexpr uint32_t kAspectPosition = 0x100;  // DIDOI_ASPECTPOSITION

struct Guid {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
    bool operator==(const Guid& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
constexpr Guid kSysMouse = {0x6f1d2b60, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr Guid kSysKeyboard = {0x6f1d2b61, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr Guid kXAxis = {0xa36d02e0, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr Guid kYAxis = {0xa36d02e1, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr Guid kZAxis = {0xa36d02e2, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr Guid kButtonGuid = {0xa36d02f0, 0xc9f3, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr Guid kKeyGuid = {0x55728220, 0xd33c, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
// {25e609e4-b259-11cf-bfc7-444553540000} CLSID_DirectInput8
constexpr Guid kClsidDirectInput8 = {0x25e609e4, 0xb259, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

struct Object {
    Guid guid;
    uint32_t type;     // DIDFT_* | instance << 8
    uint32_t ofs;      // offset in the device's default data format
    uint32_t flags;    // DIDOI_*
    std::string name;
    uint8_t dik;       // keyboard: key code
};

enum class Kind { Keyboard, Mouse };

struct Device {
    Kind kind;
    Guid guid;
    uint32_t dev_type;  // DI8DEVTYPE_* | subtype << 8
    const char* name;
    std::vector<Object> objects;
    // Installed data format: object index -> offset in the application's buffer, or -1.
    std::vector<int32_t> format_ofs;
    uint32_t data_size = 0;
    bool acquired = false;
};

const char* KeyName(uint8_t dik) {
    static const struct {
        uint8_t dik;
        const char* name;
    } kNames[] = {
        {0x01, "Esc"}, {0x0c, "-"}, {0x0d, "="}, {0x0e, "Backspace"}, {0x0f, "Tab"}, {0x1a, "["}, {0x1b, "]"},
        {0x1c, "Enter"}, {0x1d, "Ctrl"}, {0x27, ";"}, {0x28, "'"}, {0x29, "`"}, {0x2a, "Shift"}, {0x2b, "\\"},
        {0x33, ","}, {0x34, "."}, {0x35, "/"}, {0x36, "Right Shift"}, {0x37, "Num *"}, {0x38, "Alt"},
        {0x39, "Space"}, {0x3a, "Caps Lock"}, {0x45, "Num Lock"}, {0x46, "Scroll Lock"}, {0x47, "Num 7"},
        {0x48, "Num 8"}, {0x49, "Num 9"}, {0x4a, "Num -"}, {0x4b, "Num 4"}, {0x4c, "Num 5"}, {0x4d, "Num 6"},
        {0x4e, "Num +"}, {0x4f, "Num 1"}, {0x50, "Num 2"}, {0x51, "Num 3"}, {0x52, "Num 0"}, {0x53, "Num Del"},
        {0x57, "F11"}, {0x58, "F12"}, {0x9c, "Num Enter"}, {0x9d, "Right Ctrl"}, {0xb5, "Num /"},
        {0xb8, "Right Alt"}, {0xc5, "Pause"}, {0xc7, "Home"}, {0xc8, "Up"}, {0xc9, "Page Up"}, {0xcb, "Left"},
        {0xcd, "Right"}, {0xcf, "End"}, {0xd0, "Down"}, {0xd1, "Page Down"}, {0xd2, "Insert"}, {0xd3, "Delete"},
    };
    for (const auto& n : kNames) {
        if (n.dik == dik) return n.name;
    }
    static char buf[256][8];
    char* b = buf[dik];
    if (dik >= 0x02 && dik <= 0x0b) snprintf(b, 8, "%d", (dik - 1) % 10);
    else if (dik >= 0x3b && dik <= 0x44) snprintf(b, 8, "F%d", dik - 0x3a);
    else {
        static const char kRows[] = "QWERTYUIOP\0\0\0\0ASDFGHJKL\0\0\0\0\0ZXCVBNM";
        uint8_t i = uint8_t(dik - 0x10);
        if (dik >= 0x10 && i < sizeof(kRows) && kRows[i]) snprintf(b, 8, "%c", kRows[i]);
        else snprintf(b, 8, "Key %02x", dik);
    }
    return b;
}

Device MakeKeyboard() {
    Device d{Kind::Keyboard, kSysKeyboard, 0x13 | 4 << 8, "Keyboard", {}, {}};  // DI8DEVTYPE_KEYBOARD, PCENH
    for (uint8_t dik : KeyboardDiks()) {
        d.objects.push_back({kKeyGuid, kPshButton | uint32_t(dik) << 8, dik, 0, KeyName(dik), dik});
    }
    return d;
}

Device MakeMouse() {
    Device d{Kind::Mouse, kSysMouse, 0x12 | 2 << 8, "Mouse", {}, {}};  // DI8DEVTYPE_MOUSE, TRADITIONAL
    d.objects.push_back({kXAxis, kRelAxis | 0 << 8, 0, kAspectPosition, "X-axis", 0});
    d.objects.push_back({kYAxis, kRelAxis | 1 << 8, 4, kAspectPosition, "Y-axis", 0});
    d.objects.push_back({kZAxis, kRelAxis | 2 << 8, 8, kAspectPosition, "Wheel", 0});
    for (uint32_t i = 0; i < 5; ++i) {
        d.objects.push_back({kButtonGuid, kPshButton | (3 + i) << 8, 12 + i, 0, "Button " + std::to_string(i), 0});
    }
    return d;
}

std::recursive_mutex g_mutex;
std::vector<Device*> g_devices;  // index 0: unused (the IDirectInput8A object)
uint32_t g_vtable_di = 0, g_vtable_dev = 0;
std::vector<Device> g_attached;  // devices EnumDevices reports

Device* Dev(uint32_t guest) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_devices[R32(guest + 4)];
}

uint32_t NewGuest(uint32_t vtable, Device* d) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    uint32_t g = HeapAlloc(16);
    W32(g, vtable);
    W32(g + 4, uint32_t(g_devices.size()));
    g_devices.push_back(d);
    return g;
}

// DIDEVICEINSTANCEA (580 bytes).
void FillDeviceInstance(uint32_t out, const Device& d) {
    memset(Ptr<void>(out + 4), 0, 576);
    W32(out, 580);
    memcpy(Ptr<void>(out + 4), &d.guid, 16);   // guidInstance
    memcpy(Ptr<void>(out + 20), &d.guid, 16);  // guidProduct
    W32(out + 36, d.dev_type);
    strncpy(Str(out + 40), d.name, 259);   // tszInstanceName
    strncpy(Str(out + 300), d.name, 259);  // tszProductName
}

// DIDEVICEOBJECTINSTANCEA (316 bytes).
void FillObjectInstance(uint32_t out, const Object& o, uint32_t ofs) {
    uint32_t size = R32(out) >= 316 ? 316 : R32(out) ? R32(out) : 316;
    memset(Ptr<void>(out + 4), 0, size - 4);
    W32(out, size);
    memcpy(Ptr<void>(out + 4), &o.guid, 16);
    W32(out + 20, ofs);
    W32(out + 24, o.type);
    W32(out + 28, o.flags);
    strncpy(Str(out + 32), o.name.c_str(), 259);
}

bool TypeMatches(const Object& o, uint32_t filter) {
    // DIDFT_ALL (0) or any of the requested type classes.
    uint32_t classes = filter & 0xff;
    if (classes == 0) return true;
    if ((classes & kAxis) && (o.type & kAxis)) return true;
    if ((classes & kButton) && (o.type & kButton)) return true;
    return false;
}

#define M(name) void name(Ctx* c)

// ---- IUnknown ----
M(QueryInterface) {
    W32(Arg(c, 2), 0);
    Return(c, kE_NoInterface, 3);
}
M(AddRef) { Return(c, 1, 1); }
M(Release) { Return(c, 0, 1); }  // objects live for the whole run

// ---- IDirectInput8A ----
M(CreateDevice) {
    // (this, rguid, LPDIRECTINPUTDEVICE8A*, pUnkOuter)
    const Guid& g = *Ptr<Guid>(Arg(c, 1));
    for (const Device& d : g_attached) {
        if (d.guid == g) {
            W32(Arg(c, 2), NewGuest(g_vtable_dev, new Device(d)));
            Log("dinput: CreateDevice(%s)", d.name);
            Return(c, kDI_OK, 4);
            return;
        }
    }
    W32(Arg(c, 2), 0);
    Return(c, kDIERR_DeviceNotReg, 4);
}
M(EnumDevices) {
    // (this, dwDevType, callback, pvRef, dwFlags); dwDevType: DI8DEVCLASS_ALL 0, POINTER 2, KEYBOARD 3, GAMECTRL 4
    uint32_t cls = Arg(c, 1), cb = Arg(c, 2), ref = Arg(c, 3);
    uint32_t inst = HeapAlloc(580);
    for (const Device& d : g_attached) {
        uint32_t type = d.dev_type & 0xff;
        bool match = cls == 0 || cls == 1 || (cls == 2 && type == 0x12) || (cls == 3 && type == 0x13) ||
                     (cls == 4 && type >= 0x14) || cls == type;
        if (!match) continue;
        FillDeviceInstance(inst, d);
        if (GuestCall(cb, {inst, ref}) == 0) break;  // DIENUM_STOP
    }
    HeapFree(inst);
    Return(c, kDI_OK, 5);
}
M(GetDeviceStatus) { Return(c, kDI_OK, 2); }  // attached
M(RunControlPanel) { Return(c, kDI_OK, 3); }
M(DiInitialize) { Return(c, kDI_OK, 3); }
M(FindDevice) { Return(c, kDIERR_DeviceNotReg, 4); }
M(EnumDevicesBySemantics) { Return(c, kDI_OK, 6); }
M(ConfigureDevices) { Return(c, kDIERR_Unsupported, 5); }

// ---- IDirectInputDevice8A ----
M(GetCapabilities) {
    // DIDEVCAPS: dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, ...
    Device* d = Dev(Arg(c, 0));
    uint32_t out = Arg(c, 1);
    uint32_t axes = 0, buttons = 0;
    for (const Object& o : d->objects) (o.type & kAxis) ? ++axes : ++buttons;
    memset(Ptr<void>(out + 4), 0, 40);
    W32(out + 4, 1);  // DIDC_ATTACHED
    W32(out + 8, d->dev_type);
    W32(out + 12, axes);
    W32(out + 16, buttons);
    Return(c, kDI_OK, 2);
}
M(EnumObjects) {
    // (this, callback, pvRef, dwFlags)
    Device* d = Dev(Arg(c, 0));
    uint32_t cb = Arg(c, 1), ref = Arg(c, 2), filter = Arg(c, 3);
    uint32_t inst = HeapAlloc(316);
    for (const Object& o : d->objects) {
        if (!TypeMatches(o, filter)) continue;
        W32(inst, 316);
        FillObjectInstance(inst, o, o.ofs);
        if (GuestCall(cb, {inst, ref}) == 0) break;
    }
    HeapFree(inst);
    Return(c, kDI_OK, 4);
}
M(GetProperty) {
    // (this, rguidProp, DIPROPHEADER*); properties are MAKEDIPROP(n) small integers.
    Device* d = Dev(Arg(c, 0));
    uint32_t prop = Arg(c, 1), hdr = Arg(c, 2);
    switch (prop) {
        case 1:  // DIPROP_BUFFERSIZE
        case 15:  // DIPROP_JOYSTICKID
            W32(hdr + 16, 0);
            Return(c, kDI_OK, 3);
            return;
        case 2:  // DIPROP_AXISMODE: relative
            W32(hdr + 16, 1);
            Return(c, kDI_OK, 3);
            return;
        case 13:  // DIPROP_INSTANCENAME
        case 14: {  // DIPROP_PRODUCTNAME (DIPROPSTRING: WCHAR wsz[260])
            const char* s = d->name;
            for (int i = 0; i < 259 && (i == 0 || s[i - 1]); ++i) W16(hdr + 16 + 2 * uint32_t(i), uint8_t(s[i]));
            Return(c, kDI_OK, 3);
            return;
        }
        default:
            Return(c, kDIERR_Unsupported, 3);
    }
}
M(SetProperty) { Return(c, kDI_OK, 3); }
M(Acquire) {
    Dev(Arg(c, 0))->acquired = true;
    Return(c, kDI_OK, 1);
}
M(Unacquire) {
    Dev(Arg(c, 0))->acquired = false;
    Return(c, kDI_OK, 1);
}
M(GetDeviceState) {
    // (this, cbData, lpvData): values of the objects at the offsets of the installed data format.
    Device* d = Dev(Arg(c, 0));
    uint32_t size = Arg(c, 1), out = Arg(c, 2);
    memset(Ptr<void>(out), 0, size);
    int32_t dx = 0, dy = 0, dz = 0;
    uint32_t buttons = 0;
    if (d->kind == Kind::Mouse) {
        TakeMouseMotion(&dx, &dy, &dz);
        buttons = MouseButtonMask();
    }
    for (size_t i = 0; i < d->objects.size(); ++i) {
        int32_t ofs = i < d->format_ofs.size() ? d->format_ofs[i] : int32_t(d->objects[i].ofs);
        if (ofs < 0) continue;
        const Object& o = d->objects[i];
        if (o.type & kAxis) {
            if (uint32_t(ofs) + 4 > size) continue;
            int32_t v = o.ofs == 0 ? dx : o.ofs == 4 ? dy : dz;
            W32(out + uint32_t(ofs), uint32_t(v));
        } else {
            if (uint32_t(ofs) >= size) continue;
            bool down = d->kind == Kind::Keyboard ? DikDown(o.dik) : (buttons >> (o.ofs - 12)) & 1;
            W8(out + uint32_t(ofs), down ? 0x80 : 0);
        }
    }
    Return(c, kDI_OK, 3);
}
M(GetDeviceData) {
    // Buffered input is not provided: no events.
    if (Arg(c, 3)) W32(Arg(c, 3), 0);
    Return(c, kDI_OK, 5);
}
M(SetDataFormat) {
    // DIDATAFORMAT: dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf -> DIOBJECTDATAFORMAT
    // {const GUID* pguid, dwOfs, dwType, dwFlags}
    Device* d = Dev(Arg(c, 0));
    uint32_t f = Arg(c, 1);
    uint32_t n = R32(f + 16), objs = R32(f + 20);
    d->data_size = R32(f + 12);
    d->format_ofs.assign(d->objects.size(), -1);
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t od = objs + 16 * k;
        uint32_t pguid = R32(od), ofs = R32(od + 4), type = R32(od + 8);
        bool any_instance = (type & kAnyInstance) == kAnyInstance;
        for (size_t i = 0; i < d->objects.size(); ++i) {
            const Object& o = d->objects[i];
            if (d->format_ofs[i] >= 0) continue;
            if ((type & 0xff) && !(type & o.type & 0xff)) continue;
            if (!any_instance && ((type >> 8) & 0xffff) != ((o.type >> 8) & 0xffff)) continue;
            if (pguid && !(*Ptr<Guid>(pguid) == o.guid)) continue;
            d->format_ofs[i] = int32_t(ofs);
            break;
        }
    }
    Return(c, kDI_OK, 2);
}
M(SetEventNotification) { Return(c, kDI_OK, 2); }
M(SetCooperativeLevel) { Return(c, kDI_OK, 3); }
M(GetObjectInfo) {
    // (this, DIDEVICEOBJECTINSTANCEA*, dwObj, dwHow): DIPH_BYOFFSET 1 (data format offset), DIPH_BYID 2
    Device* d = Dev(Arg(c, 0));
    uint32_t out = Arg(c, 1), obj = Arg(c, 2), how = Arg(c, 3);
    for (size_t i = 0; i < d->objects.size(); ++i) {
        const Object& o = d->objects[i];
        int32_t ofs = i < d->format_ofs.size() ? d->format_ofs[i] : int32_t(o.ofs);
        if ((how == 1 && ofs == int32_t(obj)) || (how == 2 && (o.type & 0xffffff) == (obj & 0xffffff))) {
            FillObjectInstance(out, o, uint32_t(ofs));
            Return(c, kDI_OK, 4);
            return;
        }
    }
    Return(c, kDIERR_ObjectNotFound, 4);
}
M(GetDeviceInfo) {
    FillDeviceInstance(Arg(c, 1), *Dev(Arg(c, 0)));
    Return(c, kDI_OK, 2);
}
M(DevRunControlPanel) { Return(c, kDI_OK, 3); }
M(DevInitialize) { Return(c, kDI_OK, 4); }
M(CreateEffect) {
    if (Arg(c, 3)) W32(Arg(c, 3), 0);
    Return(c, kDIERR_Unsupported, 5);
}
M(EnumEffects) { Return(c, kDI_OK, 4); }
M(GetEffectInfo) { Return(c, kDIERR_Unsupported, 3); }
M(GetForceFeedbackState) { Return(c, kDIERR_Unsupported, 2); }
M(SendForceFeedbackCommand) { Return(c, kDIERR_Unsupported, 2); }
M(EnumCreatedEffectObjects) { Return(c, kDI_OK, 4); }
M(Escape) { Return(c, kDIERR_Unsupported, 2); }
M(Poll) { Return(c, 1, 1); }  // DI_NOEFFECT: not a polled device
M(SendDeviceData) { Return(c, kDIERR_Unsupported, 5); }
M(EnumEffectsInFile) { Return(c, kDI_OK, 5); }
M(WriteEffectToFile) { Return(c, kDIERR_Unsupported, 5); }
M(BuildActionMap) { Return(c, kDIERR_Unsupported, 4); }
M(SetActionMap) { Return(c, kDIERR_Unsupported, 4); }
M(GetImageInfo) { Return(c, kDIERR_Unsupported, 2); }

#undef M

const ImportFn kDiMethods[] = {
    QueryInterface, AddRef, Release, CreateDevice, EnumDevices, GetDeviceStatus, RunControlPanel, DiInitialize,
    FindDevice, EnumDevicesBySemantics, ConfigureDevices,
};
const ImportFn kDevMethods[] = {
    QueryInterface, AddRef, Release, GetCapabilities, EnumObjects, GetProperty, SetProperty, Acquire, Unacquire,
    GetDeviceState, GetDeviceData, SetDataFormat, SetEventNotification, SetCooperativeLevel, GetObjectInfo,
    GetDeviceInfo, DevRunControlPanel, DevInitialize, CreateEffect, EnumEffects, GetEffectInfo,
    GetForceFeedbackState, SendForceFeedbackCommand, EnumCreatedEffectObjects, Escape, Poll, SendDeviceData,
    EnumEffectsInFile, WriteEffectToFile, BuildActionMap, SetActionMap, GetImageInfo,
};

std::vector<ImportDef> g_defs;

uint32_t BuildVtable(const char* prefix, const ImportFn* fns, size_t n) {
    uint32_t vt = HeapAlloc(uint32_t(4 * n));
    for (size_t k = 0; k < n; ++k) {
        std::string name = std::string(prefix) + "#" + std::to_string(k);
        g_defs.push_back({"dinput8.com", strdup(name.c_str()), fns[k]});
        RegisterImports(&g_defs.back(), 1);
        W32(vt + uint32_t(4 * k), ImportThunk("dinput8.com", g_defs.back().name));
    }
    return vt;
}

void Init() {
    static std::once_flag once;
    std::call_once(once, [] {
        constexpr size_t kD = sizeof(kDiMethods) / sizeof(ImportFn), kV = sizeof(kDevMethods) / sizeof(ImportFn);
        g_defs.reserve(kD + kV);  // ImportDef pointers must stay valid
        g_vtable_di = BuildVtable("IDirectInput8A", kDiMethods, kD);
        g_vtable_dev = BuildVtable("IDirectInputDevice8A", kDevMethods, kV);
        g_devices.push_back(nullptr);
        g_attached.push_back(MakeKeyboard());
        g_attached.push_back(MakeMouse());
    });
}

}  // namespace

// CoCreateInstance hook (misc.cpp): true when `clsid` is CLSID_DirectInput8.
bool CreateDirectInput8(uint32_t clsid, uint32_t out, uint32_t* hr) {
    if (!(*Ptr<Guid>(clsid) == kClsidDirectInput8)) return false;
    Init();
    if (!out) {
        *hr = kE_Pointer;
        return true;
    }
    W32(out, NewGuest(g_vtable_di, nullptr));
    *hr = kDI_OK;
    return true;
}

}  // namespace native
