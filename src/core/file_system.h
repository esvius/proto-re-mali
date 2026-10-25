// core file system globals and the shared I/O buffer that drives use for reads.
#pragma once

#include <cstdint>

namespace core {

// Original: FUN_1064a130 (rva 0x0064a130), no arguments. Returns the file system's I/O buffer size, the third
// argument given to file system initialisation (FUN_10649f90) and stored at rva 0x0127ca14.
uint32_t FileSystemIoBufferSize();

struct CementLibrary;
struct Mutex;

// Embedded at +0x14 in a drive: the .rcf libraries registered on that drive (two singly linked lists, selected by
// the library's list index at +0x6c, linked through the library's +0x68) and the drive's shared I/O buffer, which
// FindAndOpen hands to every file range.
struct IoBufferHolder {
    CementLibrary* head[2];   // +0x00
    CementLibrary* tail[2];   // +0x08
    CementLibrary* cursor;    // +0x10 iteration position, valid between BeginIteration and EndIteration
    uint8_t lock[24];         // +0x14 Mutex (CRITICAL_SECTION)
    void* buffer;             // +0x2c
    uint32_t size;            // +0x30
    int32_t references;       // +0x34
    uint32_t heap;            // +0x38
};
static_assert(sizeof(IoBufferHolder) == 0x3c, "IoBufferHolder layout");

// Library fields used by the holder.
constexpr uint32_t kLibraryNextOffset = 0x68;
constexpr uint32_t kLibraryListIndexOffset = 0x6c;

// Original: FUN_10650b60 (rva 0x00650b60), thiscall. Clears the lists and buffer fields, calls FUN_106447f0 (as the
// original does; its result is unused) and initialises the lock. Returns this.
IoBufferHolder* __thiscall IoBufferHolderInit(IoBufferHolder* self);

// Original: FUN_10650540 (rva 0x00650540), thiscall. Calls slot 1 (release) on every registered library of both
// lists, reading each one's next link before the call, then destroys the lock.
void __thiscall IoBufferHolderReleaseLibraries(IoBufferHolder* self);

// Original: FUN_10650580 (rva 0x00650580), thiscall, ret 4. Appends `library` to the tail of its list, under the lock.
void __thiscall IoBufferHolderRegister(IoBufferHolder* self, CementLibrary* library);

// Original: FUN_106505d0 (rva 0x006505d0), thiscall, ret 4. Unlinks `library` from its list under the lock; moves the
// iteration cursor past it if it was current; fixes the tail when the removed entry was the last.
void __thiscall IoBufferHolderUnregister(IoBufferHolder* self, CementLibrary* library);

// Iteration: FUN_10650640 (ret 4) locks and sets the cursor to the head of `list`; FUN_10650660 returns the current
// library and advances; FUN_10650670 clears the cursor and unlocks.
void __thiscall IoBufferHolderBeginIteration(IoBufferHolder* self, uint32_t list);
CementLibrary* __thiscall IoBufferHolderNext(IoBufferHolder* self);
void __thiscall IoBufferHolderEndIteration(IoBufferHolder* self);

// Original: FUN_106506f0 (rva 0x006506f0), thiscall. Drops one buffer reference; the last one frees the buffer to its
// heap and clears buffer and size.
void __thiscall IoBufferHolderReleaseBuffer(IoBufferHolder* self);

// Original: FUN_106506b0 (rva 0x006506b0), thiscall, ret 0Ch. On the first reference allocates the buffer from
// `heap`: size = RoundUp(FileSystemIoBufferSize(), granularity), Alloc(heap, size, alignment). Always counts the
// reference.
void __thiscall IoBufferHolderAcquire(IoBufferHolder* self, uint32_t heap, uint32_t alignment, uint32_t granularity);

}  // namespace core
