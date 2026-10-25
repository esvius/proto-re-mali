// ADVAPI32 for the native host: an in-memory registry seeded with the game's settings (language as written by
// the installer; Language Packs/*.reg), and CryptoAPI stubs.
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "win.h"

namespace native {

namespace {

struct Value {
    uint32_t type;
    std::string data;
};

std::mutex g_mutex;
std::map<std::string, std::map<std::string, Value>> g_keys;  // lower-case full path -> values
std::map<uint32_t, std::string> g_open;                       // handle -> path
uint32_t g_next = 0x2000;

std::string Lower(std::string s) {
    for (char& ch : s) ch = char(tolower((unsigned char)ch));
    return s;
}

std::string RootName(uint32_t h) {
    switch (h) {
        case 0x80000000u: return "hkcr";
        case 0x80000001u: return "hkcu";
        case 0x80000002u: return "hklm";
        case 0x80000003u: return "hku";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_open.find(h);
    return it == g_open.end() ? std::string() : it->second;
}

void Seed() {
    static bool done = false;
    if (done) return;
    done = true;
    auto sz = [](const char* s) { return Value{1, std::string(s) + '\0'}; };
    auto& k = g_keys["hklm\\software\\activision\\prototype"];
    // English defaults; the Russian text overlay keeps LanguageCode ENU (it patches the English archives).
    k["fe_language"] = sz("69");
    k["languagecode"] = sz("ENU");
    k["lcid"] = sz("1033");
}

uint32_t Open(uint32_t parent, uint32_t sub, bool create) {
    Seed();
    std::string path = RootName(parent);
    if (path.empty()) return 0;
    if (sub && *Str(sub)) path += "\\" + Lower(Str(sub));
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_keys.count(path)) {
        if (!create) return 0;
        g_keys[path];
    }
    uint32_t h = g_next;
    g_next += 4;
    g_open[h] = path;
    return h;
}

#define A(name) void a_##name(Ctx* c)

A(RegOpenKeyExA) {
    uint32_t h = Open(Arg(c, 0), Arg(c, 1), false);
    if (h) W32(Arg(c, 4), h);
    Return(c, h ? 0 : 2, 5);  // ERROR_FILE_NOT_FOUND
}
A(RegOpenKeyA) {
    uint32_t h = Open(Arg(c, 0), Arg(c, 1), false);
    if (h) W32(Arg(c, 2), h);
    Return(c, h ? 0 : 2, 3);
}
A(RegCreateKeyExA) {
    uint32_t h = Open(Arg(c, 0), Arg(c, 1), true);
    W32(Arg(c, 7), h);
    if (Arg(c, 8)) W32(Arg(c, 8), 2);  // REG_OPENED_EXISTING_KEY
    Return(c, h ? 0 : 2, 9);
}
A(RegCloseKey) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_open.erase(Arg(c, 0));
    Return(c, 0, 1);
}
A(RegQueryValueExA) {
    std::string path = RootName(Arg(c, 0));
    std::string name = Arg(c, 1) ? Lower(Str(Arg(c, 1))) : "";
    uint32_t type_out = Arg(c, 3), data = Arg(c, 4), size_out = Arg(c, 5);
    static const bool trace = SettingInt("trace_registry", 0) != 0;
    if (trace) Log("RegQueryValueExA(%s\\%s)", path.c_str(), name.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    auto k = g_keys.find(path);
    if (k == g_keys.end() || !k->second.count(name)) {
        Return(c, 2, 6);
        return;
    }
    const Value& v = k->second[name];
    if (type_out) W32(type_out, v.type);
    uint32_t need = uint32_t(v.data.size());
    if (data) {
        if (!size_out || R32(size_out) < need) {
            if (size_out) W32(size_out, need);
            Return(c, 234, 6);  // ERROR_MORE_DATA
            return;
        }
        memcpy(Ptr<void>(data), v.data.data(), need);
    }
    if (size_out) W32(size_out, need);
    Return(c, 0, 6);
}
A(RegSetValueExA) {
    std::string path = RootName(Arg(c, 0));
    std::string name = Arg(c, 1) ? Lower(Str(Arg(c, 1))) : "";
    static const bool trace = SettingInt("trace_registry", 0) != 0;
    if (trace) Log("RegSetValueExA(%s\\%s, type %u, %u bytes, dword %u)", path.c_str(), name.c_str(), Arg(c, 3),
                   Arg(c, 5), Arg(c, 5) >= 4 ? R32(Arg(c, 4)) : 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_keys[path][name] = Value{Arg(c, 3), std::string(Str(Arg(c, 4)), Arg(c, 5))};
    Return(c, 0, 6);
}
A(RegEnumKeyA) { Return(c, 259, 4); }  // ERROR_NO_MORE_ITEMS

// CryptoAPI: the engine uses it for online features and random seeds only.
A(CryptAcquireContextA) {
    W32(Arg(c, 0), 0x3000);
    Return(c, 1, 5);
}
A(CryptReleaseContext) { Return(c, 1, 2); }
A(CryptGenRandom) {
    uint32_t buf = Arg(c, 2);
    for (uint32_t i = 0; i < Arg(c, 1); ++i) W8(buf + i, uint8_t(rand()));
    Return(c, 1, 3);
}
A(CryptCreateHash) { Return(c, 0, 5); }
A(CryptHashData) { Return(c, 0, 4); }
A(CryptGetHashParam) { Return(c, 0, 5); }
A(CryptDestroyHash) { Return(c, 1, 1); }
A(CryptImportKey) { Return(c, 0, 6); }
A(CryptEncrypt) { Return(c, 0, 7); }
A(CryptDestroyKey) { Return(c, 1, 1); }

#undef A

#define E(name) {"advapi32.dll", #name, a_##name}
const ImportDef kAdvapi32[] = {
    E(RegOpenKeyExA), E(RegOpenKeyA), E(RegCreateKeyExA), E(RegCloseKey), E(RegQueryValueExA), E(RegSetValueExA),
    E(RegEnumKeyA), E(CryptAcquireContextA), E(CryptReleaseContext), E(CryptGenRandom), E(CryptCreateHash),
    E(CryptHashData), E(CryptGetHashParam), E(CryptDestroyHash), E(CryptImportKey), E(CryptEncrypt),
    E(CryptDestroyKey),
};
#undef E
ImportRegistrar g_advapi32(kAdvapi32);

}  // namespace

}  // namespace native
