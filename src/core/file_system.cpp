#include "file_system.h"

#include "../host/hooks.h"

namespace core {

namespace {

constexpr uint32_t kIoBufferSizeRva = 0x0127ca14;

// Engine entry points called through their live addresses (reconstructed or not).
constexpr uint32_t kFileSystemIoBufferSizeRva = 0x0064a130;
constexpr uint32_t kRoundUpRva = 0x00645300;
constexpr uint32_t kAllocRva = 0x0063a8d0;
constexpr uint32_t kFreeRva = 0x0063a9c0;
constexpr uint32_t kMutexInitRva = 0x0063d260;
constexpr uint32_t kMutexDestroyRva = 0x0063d270;
constexpr uint32_t kMutexUnlockRva = 0x0063d280;
constexpr uint32_t kMutexLockRva = 0x0063d290;
constexpr uint32_t kConstructionMarkerRva = 0x006447f0;  // returns a constant pointer; called by constructors

constexpr int kLibrarySlotRelease = 1;

using IoBufferSizeFn = uint32_t(__cdecl*)();
using RoundUpFn = uint32_t(__cdecl*)(uint32_t, uint32_t);
using AllocFn = void*(__cdecl*)(uint32_t, uint32_t, uint32_t);
using FreeFn = void(__cdecl*)(uint32_t, void*);
using MutexFn = void(__cdecl*)(void*);
using MarkerFn = const void*(__cdecl*)();
using ReleaseFn = void(__thiscall*)(CementLibrary*);

template <typename Fn>
Fn Engine(uint32_t rva) {
    return reinterpret_cast<Fn>(protore::EngineAddress(rva));
}

CementLibrary*& Next(CementLibrary* library) {
    return *reinterpret_cast<CementLibrary**>(reinterpret_cast<uint8_t*>(library) + kLibraryNextOffset);
}

uint32_t ListIndex(CementLibrary* library) {
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(library) + kLibraryListIndexOffset);
}

void Lock(IoBufferHolder* h) {
    Engine<MutexFn>(kMutexLockRva)(h->lock);
}

void Unlock(IoBufferHolder* h) {
    Engine<MutexFn>(kMutexUnlockRva)(h->lock);
}

}  // namespace

uint32_t FileSystemIoBufferSize() {
    return *static_cast<uint32_t*>(protore::EngineAddress(kIoBufferSizeRva));
}

void __thiscall IoBufferHolderAcquire(IoBufferHolder* self, uint32_t heap, uint32_t alignment, uint32_t granularity) {
    if (self->references == 0) {
        self->heap = heap;
        uint32_t size = Engine<RoundUpFn>(kRoundUpRva)(Engine<IoBufferSizeFn>(kFileSystemIoBufferSizeRva)(),
                                                       granularity);
        self->size = size;
        self->buffer = Engine<AllocFn>(kAllocRva)(self->heap, size, alignment);
    }
    ++self->references;
}

IoBufferHolder* __thiscall IoBufferHolderInit(IoBufferHolder* self) {
    self->cursor = nullptr;
    self->buffer = nullptr;
    self->size = 0;
    self->references = 0;
    self->heap = 0;
    Engine<MarkerFn>(kConstructionMarkerRva)();
    self->head[0] = nullptr;
    self->tail[0] = nullptr;
    self->head[1] = nullptr;
    self->tail[1] = nullptr;
    Engine<MutexFn>(kMutexInitRva)(self->lock);
    return self;
}

void __thiscall IoBufferHolderReleaseLibraries(IoBufferHolder* self) {
    for (uint32_t list = 0; list < 2; ++list) {
        for (CementLibrary* library = self->head[list]; library;) {
            CementLibrary* next = Next(library);
            void** vtable = *reinterpret_cast<void***>(library);
            reinterpret_cast<ReleaseFn>(vtable[kLibrarySlotRelease])(library);
            library = next;
        }
    }
    Engine<MutexFn>(kMutexDestroyRva)(self->lock);
}

void __thiscall IoBufferHolderRegister(IoBufferHolder* self, CementLibrary* library) {
    Lock(self);
    uint32_t list = ListIndex(library);
    if (!self->head[list]) {
        self->head[list] = library;
    } else {
        Next(self->tail[list]) = library;
    }
    Next(library) = nullptr;
    self->tail[list] = library;
    Unlock(self);
}

void __thiscall IoBufferHolderUnregister(IoBufferHolder* self, CementLibrary* library) {
    Lock(self);
    uint32_t list = ListIndex(library);
    CementLibrary** link = &self->head[list];
    CementLibrary* previous = nullptr;
    if (*link) {
        for (;;) {
            CementLibrary* current = *link;
            if (current == library) {
                if (library == self->cursor) {
                    self->cursor = Next(self->cursor);
                }
                *link = Next(*link);
                break;
            }
            link = &Next(current);
            previous = current;
            if (!*link) {
                break;
            }
        }
        if (*link) {
            Unlock(self);
            return;
        }
    }
    self->tail[list] = previous;
    Unlock(self);
}

void __thiscall IoBufferHolderBeginIteration(IoBufferHolder* self, uint32_t list) {
    Lock(self);
    self->cursor = self->head[list];
}

CementLibrary* __thiscall IoBufferHolderNext(IoBufferHolder* self) {
    CementLibrary* current = self->cursor;
    if (current) {
        self->cursor = Next(current);
    }
    return current;
}

void __thiscall IoBufferHolderEndIteration(IoBufferHolder* self) {
    self->cursor = nullptr;
    Unlock(self);
}

void __thiscall IoBufferHolderReleaseBuffer(IoBufferHolder* self) {
    if (self->references == 1) {
        Engine<FreeFn>(kFreeRva)(self->heap, self->buffer);
        self->buffer = nullptr;
        self->size = 0;
    }
    --self->references;
}

}  // namespace core
