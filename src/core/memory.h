// core memory: heap-indexed allocation (code\core\src\memory). Every engine allocation goes through Alloc.
#pragma once

#include <cstdint>

namespace core {

// One record per heap in the engine's heap table (pointer at rva 0x010bc5a4, 16-byte records).
struct HeapRecord {
    void* allocator;  // object whose vtable slot 5 is Allocate(size, alignment)
    uint32_t reserved[3];
};
static_assert(sizeof(HeapRecord) == 16, "heap record is 16 bytes");

// Original: FUN_1063a8d0 (rva 0x0063a8d0), cdecl. Allocates `size` bytes from heap `heap` with at least 4-byte
// alignment. If the allocator returns null and an out-of-memory handler is registered (rva 0x01277f8c, context at
// 0x01277f90), calls handler(context, heap, size, alignment, null) before returning null.
void* Alloc(uint32_t heap, uint32_t size, uint32_t alignment);

// Original: FUN_1063a9c0 (rva 0x0063a9c0), cdecl. Returns memory from Alloc to heap `heap` (allocator slot 6).
void Free(uint32_t heap, void* p);

// Original: FUN_1063a9a0 (rva 0x0063a9a0), cdecl. Releases an object allocated from heap `heap` (allocator slot 4);
// used by deleting destructors.
void FreeObject(uint32_t heap, void* p);

// Original: FUN_10645300 (rva 0x00645300), cdecl. Rounds `value` up to a multiple of `granularity`; returns `value`
// unchanged when granularity is 0 or value is already a multiple.
uint32_t RoundUp(uint32_t value, uint32_t granularity);

}  // namespace core
