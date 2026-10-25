// core::File (code\core\src\file\common\file.cpp): a file on a drive. Every operation is asynchronous: it allocates
// a drive request (FUN_1064c4e0), constructs it and queues it on the drive (FUN_1064feb0) with the file's priority;
// "...AndWait" variants call the asynchronous slot and then the wait slot. Files come from a fixed pool with an
// index table. The object belongs to the engine, so it is described by an explicit layout.
#pragma once

#include <cstdint>

#include "event.h"

namespace core {

struct Drive;

struct File {
    void* vtable;              // +0x000
    void* lock_vtable;         // +0x004 lock sub-object: slot 0 lock, slot 1 unlock
    int32_t references;        // +0x008
    Drive* drive;              // +0x00c
    uint32_t priority;         // +0x010 passed with every queued request
    uint32_t status;           // +0x014 read under the lock
    int32_t pending;           // +0x018 requests queued and not yet finished
    CountedEvent event;        // +0x01c signalled when pending reaches 0 while someone waits
    uint8_t event_created;     // +0x024
    uint8_t waiting;           // +0x025
    uint8_t open;              // +0x026
    uint8_t flag_27;           // +0x027 constructor argument 3
    char name[256];            // +0x028
    uint8_t name_terminator;   // +0x128 zeroed when the name had to be truncated
    uint8_t pad_129[3];
    uint32_t field_12c;        // +0x12c set by FUN_1064e770
    uint32_t field_130;        // +0x130
    uint32_t field_134;        // +0x134 returned by FUN_1064e4d0
    uint8_t sub_138[0x2c];     // +0x138 cleared by FUN_10650dd0, trivial destructor FUN_10650e00
    uint8_t unknown_164[8];    // +0x164
    uint8_t flag_16c;          // +0x16c 0 for plain files, 1 for files opened on construction
    uint8_t pad_16d[3];
    uint32_t block_size;       // +0x170 0x800
};
static_assert(sizeof(File) == 0x174, "File is 0x174 bytes");

// ---- pool (globals at rva 0x0127ca60 mutex, 0x0127ca78 pool, 0x0127ca7c index table, 0x0127ca80 count) ----
File* FileAllocate(uint32_t ignored_size);  // FUN_1064e050
void FileFree(File* file);                   // FUN_1064e0b0
void FilePoolShutdown();                     // FUN_1064e850
void FileSystemLock();                       // FUN_1064e200
void FileSystemUnlock();                     // FUN_1064e210
void* __thiscall FileSubObjectInit(void* sub);  // FUN_10650dd0 (thiscall on +0x138): clears 11 words

// ---- lifetime ----
// FUN_1064e9e0, ret 28h. Optional open request (argument 9) queued when nonzero; arguments 6-8 unused.
File* __thiscall FileConstruct(File* self, Drive* drive, const char* name, uint32_t flag, uint32_t a4,
                               uint32_t priority, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t auto_open,
                               uint32_t a10);
// FUN_1064e8d0, ret 1Ch. Always queues an open request.
File* __thiscall FileConstructOpening(File* self, Drive* drive, const char* name, uint32_t flag, uint32_t a4,
                                      uint32_t a5, uint32_t a6, uint32_t priority);
void __thiscall FileDestruct(File* self);                           // FUN_1064e100
File* __thiscall FileDeletingDestruct(File* self, uint32_t flags);  // FUN_1064e8b0, ret 4
uint32_t __thiscall FileAddRef(File* self);                         // FUN_1064e150
uint32_t __thiscall FileRelease(File* self);                        // FUN_1064e170

// ---- request accounting and waiting ----
uint32_t __thiscall FileBeginRequest(File* self);                   // FUN_1064e7f0
uint32_t __thiscall FileEndRequest(File* self);                     // FUN_1064e810
bool __thiscall FileIsIdle(File* self);                             // FUN_1064e5d0 (al)
void __thiscall FileWaitPolling(File* self);                        // FUN_1064e600
void __thiscall FileWaitBlocking(File* self);                       // FUN_1064e640
bool __thiscall FileChangeDrive(File* self, Drive* drive);          // FUN_1064e790, ret 4 (al)

// ---- asynchronous operations (request size, constructor) ----
uint32_t __thiscall FileQueueD910(File* self);                                    // FUN_1064e220 (0x0c)
uint32_t __thiscall FileQueueD950(File* self, uint32_t a, uint32_t b, uint32_t c);  // FUN_1064e260 (0x18)
uint32_t __thiscall FileQueueD9B0(File* self, uint32_t a, uint32_t b, uint32_t c);  // FUN_1064e2b0 (0x18)
uint32_t __thiscall FileQueueDA10(File* self, uint32_t a);                        // FUN_1064e300 (0x10)
uint32_t __thiscall FileQueueDA60(File* self, uint32_t a);                        // FUN_1064e350 (0x10)
uint32_t __thiscall FileQueueDAB0(File* self, uint32_t a);                        // FUN_1064e3a0 (0x10)
uint32_t __thiscall FileQueueDB00(File* self);                                    // FUN_1064e4e0 (0x0c, flag 1)
uint32_t __thiscall FileQueueD670(File* self, uint32_t a, uint32_t b);            // FUN_1064e520 (0x18, TLS)
uint32_t __thiscall FileQueueD5E0(File* self, uint32_t a);                        // FUN_1064e580 (0x10)

// ---- synchronous wrappers: slot n, then slot 21 (vtable +0x54) ----
uint32_t __thiscall FileSlot3AndWait(File* self, uint32_t a, uint32_t b, uint32_t c);  // FUN_1064e3f0
uint32_t __thiscall FileSlot4AndWait(File* self, uint32_t a, uint32_t b, uint32_t c);  // FUN_1064e420
uint32_t __thiscall FileSlot5AndWait(File* self, uint32_t a);                          // FUN_1064e450
uint32_t __thiscall FileSlot6AndWait(File* self, uint32_t a);                          // FUN_1064e470
uint32_t __thiscall FileSlot7AndWait(File* self, uint32_t a);                          // FUN_1064e490
uint32_t __thiscall FileSlot8AndWait(File* self);                                      // FUN_1064e4b0

// ---- accessors ----
uint32_t __thiscall FileStatusLocked(File* self);         // FUN_1064e6b0
const char* __thiscall FileNameChecked(File* self);       // FUN_1064e6f0: null when drive slot 8 reports < 0
// FUN_1064e720: tail-jumps into drive slot 4 with the caller's stack untouched (assembly, as the original).
extern "C" void FileDriveSlot4();
uint32_t __thiscall FileField134(File* self);             // FUN_1064e4d0
uint8_t __thiscall FileFlag16c(File* self);               // FUN_1064e750
uint32_t __thiscall FileBlockSize(File* self);            // FUN_1064e760
void __thiscall FileSetField12c(File* self, uint32_t v);  // FUN_1064e770, ret 4
void* __thiscall FileSubObject(File* self);               // FUN_1064e780
// Too short to hook (3-4 bytes), reproduced inline where used: FUN_1064e6e0 open, FUN_1064e710 name,
// FUN_1064e730 no-op (ret 4), FUN_1064e740 priority.

}  // namespace core
