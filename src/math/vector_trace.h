// math vector debug trace (math\src\vector.cpp, lines 0xd5/0xd6): prints a vector through core::Report
// when its per-site trace flags are set. 40 call sites; flags are off in the shipped build.
#pragma once

namespace math {

// Original: FUN_107cea20 (rva 0x007cea20), __fastcall, v = float[3].
void __fastcall TraceVector(const float* v);

}  // namespace math
