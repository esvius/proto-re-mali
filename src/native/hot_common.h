// Helpers for native replacements of engine functions (hot_*.cpp, see tools/recomp/recomp.py NATIVE_OVERRIDES).
#pragma once

#include <initializer_list>

#include "guest.h"

namespace native {

// Engine addresses in the hot_*.cpp files are the DLL's (preferred base 0x10000000); the engine runs at kEngineBase.
inline uint32_t E(uint32_t a) { return a - 0x10000000u + kEngineBase; }

inline float F(uint32_t a) { return RF32(a); }
inline const float* FP(uint32_t a) { return reinterpret_cast<const float*>(G(a)); }  // no null check: hot

// Calls guest code at `va` (a guest address: E(...) for a fixed engine function, or a pointer read from guest
// memory such as a vtable slot) from native code, on the same guest stack.
inline void CallGuestRaw(Ctx* c, uint32_t va) {
    if (RecompFn fn = RecompLookup(va)) fn(c);
    else RecompDispatch(c, va);
}

// cdecl: the caller pops the arguments. Returns eax.
inline uint32_t CallGuestCdecl(Ctx* c, uint32_t va, std::initializer_list<uint32_t> args) {
    const uint32_t* a = args.end();
    while (a != args.begin()) Push32(c, *--a);
    Push32(c, 0xfffffff0u);  // return address, never jumped to: the callee only pops it
    CallGuestRaw(c, va);
    c->esp += 4 * uint32_t(args.size());
    return c->eax;
}

// thiscall: `self` in ecx, the callee pops its arguments. Returns eax.
inline uint32_t CallGuestThiscall(Ctx* c, uint32_t va, uint32_t self, std::initializer_list<uint32_t> args) {
    const uint32_t* a = args.end();
    while (a != args.begin()) Push32(c, *--a);
    Push32(c, 0xfffffff0u);
    c->ecx = self;
    CallGuestRaw(c, va);
    return c->eax;
}

}  // namespace native
