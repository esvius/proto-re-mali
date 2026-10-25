// core string/name hashing. The Cement (.rcf) directory is sorted by HashFileName of each entry's path;
// verified against all 18,044 entries of the shipped archives.
#pragma once

#include <cstdint>

namespace core {

// Original: FUN_1063a5d0 (rva 0x0063a5d0), cdecl. Multiplicative hash h = h * 31 + c over signed chars, where
// every char below 'a' is shifted up by 0x20 (folds upper case, but also shifts digits and punctuation).
// After hashing, the engine's registered hash observer is called with (hash, string). A null string returns seed.
uint32_t HashString(const char* s, uint32_t seed);

// Original: FUN_10643ed0 (rva 0x00643ed0), cdecl. HashString of a path with one leading '\' removed, seed 0.
uint32_t HashFileName(const char* path);

}  // namespace core
