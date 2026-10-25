#include "name_hash.h"

#include "../host/hooks.h"

namespace core {

namespace {

// Engine data: pointer to the hash observer, called as observer(hash, string) after every HashString.
constexpr uint32_t kHashObserverRva = 0x010bc59c;

using HashObserverFn = void(__cdecl*)(uint32_t hash, const char* s);

HashObserverFn Observer() {
    return *static_cast<HashObserverFn*>(protore::EngineAddress(kHashObserverRva));
}

}  // namespace

uint32_t HashString(const char* s, uint32_t seed) {
    if (!s) {
        return seed;
    }
    uint32_t h = seed;
    for (const char* p = s; *p; ++p) {
        int c = static_cast<signed char>(*p);
        if (c < 'a') {
            c += 0x20;
        }
        h = h * 31 + static_cast<uint32_t>(c);
    }
    Observer()(h, s);
    return h;
}

uint32_t HashFileName(const char* path) {
    if (*path == '\\') {
        ++path;
    }
    return HashString(path, 0);
}

}  // namespace core
