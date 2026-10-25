#include "memory.h"

#include "../host/hooks.h"

namespace core {

namespace {

constexpr uint32_t kHeapTableRva = 0x010bc5a4;   // HeapRecord* (array indexed by heap id)
constexpr uint32_t kOomHandlerRva = 0x01277f8c;  // OomHandlerFn, may be null
constexpr uint32_t kOomContextRva = 0x01277f90;  // first argument passed to the handler

constexpr uint32_t kMinAlignment = 4;
constexpr int kAllocatorSlotFreeObject = 4;  // vtable +0x10
constexpr int kAllocatorSlotAllocate = 5;    // vtable +0x14
constexpr int kAllocatorSlotFree = 6;        // vtable +0x18

using AllocateFn = void*(__thiscall*)(void* allocator, uint32_t size, uint32_t alignment);
using ReleaseFn = void(__thiscall*)(void* allocator, void* p);
using OomHandlerFn = void(__cdecl*)(uint32_t context, uint32_t heap, uint32_t size, uint32_t alignment, void* result);

template <typename T>
T Global(uint32_t rva) {
    return *static_cast<T*>(protore::EngineAddress(rva));
}

}  // namespace

void* Alloc(uint32_t heap, uint32_t size, uint32_t alignment) {
    if (alignment < kMinAlignment) {
        alignment = kMinAlignment;
    }
    void* allocator = Global<HeapRecord*>(kHeapTableRva)[heap].allocator;
    void** vtable = *static_cast<void***>(allocator);
    void* result = reinterpret_cast<AllocateFn>(vtable[kAllocatorSlotAllocate])(allocator, size, alignment);
    if (!result) {
        if (auto handler = Global<OomHandlerFn>(kOomHandlerRva)) {
            handler(Global<uint32_t>(kOomContextRva), heap, size, alignment, result);
        }
    }
    return result;
}

namespace {

void CallRelease(uint32_t heap, void* p, int slot) {
    void* allocator = Global<HeapRecord*>(kHeapTableRva)[heap].allocator;
    void** vtable = *static_cast<void***>(allocator);
    reinterpret_cast<ReleaseFn>(vtable[slot])(allocator, p);
}

}  // namespace

void Free(uint32_t heap, void* p) {
    CallRelease(heap, p, kAllocatorSlotFree);
}

void FreeObject(uint32_t heap, void* p) {
    CallRelease(heap, p, kAllocatorSlotFreeObject);
}

uint32_t RoundUp(uint32_t value, uint32_t granularity) {
    if (granularity == 0) {
        return value;
    }
    uint32_t remainder = value % granularity;
    if (remainder == 0) {
        return value;
    }
    return value - remainder + granularity;
}

}  // namespace core
