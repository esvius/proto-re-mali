#include "file.h"

#include <cstring>

#include "../host/hooks.h"

namespace core {

namespace {

// ---- engine globals and entry points (rva) ----
constexpr uint32_t kFileSystemMutexRva = 0x0127ca60;
constexpr uint32_t kFilePoolRva = 0x0127ca78;
constexpr uint32_t kFileTableRva = 0x0127ca7c;
constexpr uint32_t kFileCountRva = 0x0127ca80;

constexpr uint32_t kMutexLockRva = 0x0063d290;
constexpr uint32_t kMutexUnlockRva = 0x0063d280;
constexpr uint32_t kMutexDestroyRva = 0x0063d270;
constexpr uint32_t kReportOpenFilesRva = 0x0064df50;  // called when the pool is exhausted
constexpr uint32_t kFreeTableRva = 0x0063ac00;
constexpr uint32_t kSafeCopyRva = 0x0063e750;         // returns < 0 when the source did not fit
constexpr uint32_t kStrncpyImportRva = 0x01331fc8;    // MSVCR80!strncpy
constexpr uint32_t kMarkerRva = 0x006447f0;
constexpr uint32_t kSubObjectInitRva = 0x00650dd0;
constexpr uint32_t kSubObjectDestroyRva = 0x00650e00;
constexpr uint32_t kFileFreeRva = 0x0064e0b0;
constexpr uint32_t kEventCreateRva = 0x0063d320;
constexpr uint32_t kEventDestroyRva = 0x0063d350;
constexpr uint32_t kEventResetRva = 0x0063d390;
constexpr uint32_t kEventSignalRva = 0x0063d3d0;
constexpr uint32_t kEventWaitRva = 0x0063d400;
constexpr uint32_t kThreadLocalRva = 0x00644dc0;
constexpr uint32_t kSleepRva = 0x00644b40;
constexpr uint32_t kNewRequestRva = 0x0064c4e0;
constexpr uint32_t kQueueRequestRva = 0x0064feb0;

// Request constructors (FileRequest family, not reconstructed yet).
constexpr uint32_t kOpenRequestRva = 0x0064d7b0;         // (file, a4, flag, a10), size 0x18
constexpr uint32_t kOpenNowRequestRva = 0x0064d870;      // (file, a4, flag, a5, a6), size 0x1c
constexpr uint32_t kCloseRequestRva = 0x0064d8d0;        // (file), size 0x0c

constexpr uint32_t kVtableFileBase = 0x00e47c28;   // +4 before the final one
constexpr uint32_t kVtableFile = 0x00e47c84;       // +0
constexpr uint32_t kVtableFileLock = 0x00e47c78;   // +4
constexpr uint32_t kVtableDestroyed = 0x00d96db8;  // +0 after destruction

constexpr int32_t kReleasedRefCount = 500000;
constexpr uint32_t kBlockSize = 0x800;
constexpr uint32_t kSignalAll = 0xffff;

// Pool object slots.
constexpr int kPoolSlotRelease = 1;
constexpr int kPoolSlotFree = 4;
constexpr int kPoolSlotEnumerate = 11;
constexpr int kPoolSlotAllocate = 18;
constexpr int kPoolSlotIndexOf = 19;

// Drive slots used by File.
constexpr int kDriveSlotAddRef = 0;
constexpr int kDriveSlotRelease = 1;
constexpr int kDriveSlotService = 3;
constexpr int kDriveSlotNotify = 8;  // vtable +0x20, returns a signed byte

// File's own slots used through its vtable.
constexpr int kFileSlotDeleting = 2;
constexpr int kFileSlotIsIdle = 20;  // vtable +0x50
constexpr int kFileSlotWait = 21;    // vtable +0x54

using MutexFn = void(__cdecl*)(void*);
using VoidFn = void(__cdecl*)();
using FreeTableFn = void(__cdecl*)(void*);
using SafeCopyFn = int(__cdecl*)(char*, const char*, uint32_t);
using StrncpyFn = char*(__cdecl*)(char*, const char*, uint32_t);
using MarkerFn = const void*(__cdecl*)();
using SubObjectFn = void(__thiscall*)(void*);
using FileFreeFn = void(__cdecl*)(File*);
using EventCreateFn = void(__cdecl*)(CountedEvent*, uint32_t);
using EventFn = void(__cdecl*)(CountedEvent*);
using EventCountFn = void(__cdecl*)(CountedEvent*, uint32_t);
using TlsFn = void*(__cdecl*)();
using SleepFn = void(__cdecl*)(uint32_t);
using NewRequestFn = void*(__cdecl*)(uint32_t);
using QueueFn = uint32_t(__thiscall*)(Drive*, void* request, uint32_t priority, uint32_t flag);
using MethodFn = uint32_t(__thiscall*)(void*);
using Method1Fn = uint32_t(__thiscall*)(void*, uint32_t);
using Method3Fn = uint32_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
using ByteMethodFn = int8_t(__thiscall*)(void*);
using BoolMethodFn = bool(__thiscall*)(void*);
using PoolAllocateFn = File*(__thiscall*)(void*);
using PoolIndexFn = uint32_t(__thiscall*)(void*, File*);
using PoolFreeFn = void(__thiscall*)(void*, File*);
using PoolEnumerateFn = void(__thiscall*)(void*, uint32_t, uint32_t*);

template <typename Fn>
Fn Engine(uint32_t rva) {
    return reinterpret_cast<Fn>(protore::EngineAddress(rva));
}

template <typename T>
T& Global(uint32_t rva) {
    return *static_cast<T*>(protore::EngineAddress(rva));
}

template <typename Fn>
Fn Slot(void* object, int index) {
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

void* FsMutex() {
    return protore::EngineAddress(kFileSystemMutexRva);
}

void* Pool() {
    return Global<void*>(kFilePoolRva);
}

File** Table() {
    return Global<File**>(kFileTableRva);
}

// The lock sub-object at +4.
void* LockObject(File* f) {
    return &f->lock_vtable;
}

void Lock(File* f) {
    Slot<MethodFn>(LockObject(f), 0)(LockObject(f));
}

uint32_t Unlock(File* f) {
    return Slot<MethodFn>(LockObject(f), 1)(LockObject(f));
}

uint32_t DriveCall(File* f, int slot) {
    return Slot<MethodFn>(f->drive, slot)(f->drive);
}

uint32_t Queue(File* f, void* request, uint32_t flag) {
    return Engine<QueueFn>(kQueueRequestRva)(f->drive, request, f->priority, flag);
}

void* NewRequest(uint32_t size) {
    return Engine<NewRequestFn>(kNewRequestRva)(size);
}

// Shared shape of every asynchronous operation: allocate, construct when allocation succeeded, queue whatever
// came out (a null request is queued too, as in the original).
template <typename Construct>
uint32_t QueueNew(File* f, uint32_t size, uint32_t flag, Construct construct) {
    void* request = NewRequest(size);
    if (request) {
        request = construct(request);
    }
    return Queue(f, request, flag);
}

using Ctor1 = void*(__thiscall*)(void*, File*);
using Ctor2 = void*(__thiscall*)(void*, File*, uint32_t);
using Ctor4 = void*(__thiscall*)(void*, File*, uint32_t, uint32_t, uint32_t);
using Ctor5 = void*(__thiscall*)(void*, File*, uint32_t, uint32_t, uint32_t, uint32_t);

void CopyName(File* f, const char* name) {
    if (Engine<SafeCopyFn>(kSafeCopyRva)(f->name, name, sizeof(f->name)) < 0) {
        (*static_cast<StrncpyFn*>(protore::EngineAddress(kStrncpyImportRva)))(f->name, name, sizeof(f->name));
        f->name_terminator = 0;
    }
}

// Common part of both constructors up to the open request.
void ConstructCommon(File* f, Drive* drive, const char* name, uint32_t flag, uint32_t priority, uint8_t flag_16c) {
    f->lock_vtable = protore::EngineAddress(kVtableFileBase);
    f->priority = priority;
    f->vtable = protore::EngineAddress(kVtableFile);
    f->lock_vtable = protore::EngineAddress(kVtableFileLock);
    f->references = 1;
    f->drive = drive;
    f->status = 0;
    f->pending = 0;
    f->event_created = 0;
    f->waiting = 0;
    f->open = 0;
    f->flag_27 = static_cast<uint8_t>(flag);
    f->field_12c = 0;
    f->field_130 = 0;
    f->field_134 = 0;
    Engine<SubObjectFn>(kSubObjectInitRva)(f->sub_138);
    f->flag_16c = flag_16c;
    f->block_size = kBlockSize;
    Engine<MarkerFn>(kMarkerRva)();
    DriveCall(f, kDriveSlotAddRef);
    CopyName(f, name);
    DriveCall(f, kDriveSlotNotify);
}

}  // namespace

// ---- pool ---------------------------------------------------------------------------------------------------

File* FileAllocate(uint32_t /*ignored_size*/) {
    Engine<MutexFn>(kMutexLockRva)(FsMutex());
    File* file = Slot<PoolAllocateFn>(Pool(), kPoolSlotAllocate)(Pool());
    if (!file) {
        Engine<VoidFn>(kReportOpenFilesRva)();
        Engine<MutexFn>(kMutexUnlockRva)(FsMutex());
        return nullptr;
    }
    uint32_t index = Slot<PoolIndexFn>(Pool(), kPoolSlotIndexOf)(Pool(), file);
    Table()[index] = file;
    Engine<MutexFn>(kMutexUnlockRva)(FsMutex());
    return file;
}

void FileFree(File* file) {
    Engine<MutexFn>(kMutexLockRva)(FsMutex());
    uint32_t index = Slot<PoolIndexFn>(Pool(), kPoolSlotIndexOf)(Pool(), file);
    Table()[index] = nullptr;
    Slot<PoolFreeFn>(Pool(), kPoolSlotFree)(Pool(), file);
    Engine<MutexFn>(kMutexUnlockRva)(FsMutex());
}

void FilePoolShutdown() {
    Engine<MutexFn>(kMutexDestroyRva)(FsMutex());
    uint32_t enumerated = 0;
    Slot<PoolEnumerateFn>(Pool(), kPoolSlotEnumerate)(Pool(), 0, &enumerated);
    Slot<MethodFn>(Pool(), kPoolSlotRelease)(Pool());
    Global<void*>(kFilePoolRva) = nullptr;
    Engine<FreeTableFn>(kFreeTableRva)(Table());
    Global<File**>(kFileTableRva) = nullptr;
    Global<uint32_t>(kFileCountRva) = 0;
}

void FileSystemLock() {
    Engine<MutexFn>(kMutexLockRva)(FsMutex());
}

void FileSystemUnlock() {
    Engine<MutexFn>(kMutexUnlockRva)(FsMutex());
}

void* __thiscall FileSubObjectInit(void* sub) {
    memset(sub, 0, 0x2c);
    return sub;
}

// ---- lifetime -----------------------------------------------------------------------------------------------

File* __thiscall FileConstruct(File* self, Drive* drive, const char* name, uint32_t flag, uint32_t a4,
                               uint32_t priority, uint32_t /*a6*/, uint32_t /*a7*/, uint32_t /*a8*/,
                               uint32_t auto_open, uint32_t a10) {
    ConstructCommon(self, drive, name, flag, priority, 0);
    if (static_cast<uint8_t>(auto_open)) {
        QueueNew(self, 0x18, 0, [&](void* r) { return Engine<Ctor4>(kOpenRequestRva)(r, self, a4, flag, a10); });
    }
    return self;
}

File* __thiscall FileConstructOpening(File* self, Drive* drive, const char* name, uint32_t flag, uint32_t a4,
                                      uint32_t a5, uint32_t a6, uint32_t priority) {
    ConstructCommon(self, drive, name, flag, priority, 1);
    QueueNew(self, 0x1c, 0, [&](void* r) { return Engine<Ctor5>(kOpenNowRequestRva)(r, self, a4, flag, a5, a6); });
    return self;
}

void __thiscall FileDestruct(File* self) {
    self->vtable = protore::EngineAddress(kVtableFile);
    self->lock_vtable = protore::EngineAddress(kVtableFileLock);
    if (self->event_created) {
        Engine<EventFn>(kEventDestroyRva)(&self->event);
        self->event_created = 0;
    }
    DriveCall(self, kDriveSlotRelease);
    Engine<SubObjectFn>(kSubObjectDestroyRva)(self->sub_138);
    self->vtable = protore::EngineAddress(kVtableDestroyed);
}

File* __thiscall FileDeletingDestruct(File* self, uint32_t flags) {
    FileDestruct(self);
    if (flags & 1) {
        Engine<FileFreeFn>(kFileFreeRva)(self);
    }
    return self;
}

uint32_t __thiscall FileAddRef(File* self) {
    Lock(self);
    ++self->references;
    return Unlock(self);
}

uint32_t __thiscall FileRelease(File* self) {
    Lock(self);
    if (--self->references != 0) {
        return Unlock(self);
    }
    if (self->open) {
        Unlock(self);
        return QueueNew(self, 0x0c, 0, [&](void* r) { return Engine<Ctor1>(kCloseRequestRva)(r, self); });
    }
    self->references = kReleasedRefCount;
    Unlock(self);
    return Slot<Method1Fn>(self, kFileSlotDeleting)(self, 1);
}

// ---- request accounting and waiting ---------------------------------------------------------------------------

uint32_t __thiscall FileBeginRequest(File* self) {
    Lock(self);
    ++self->pending;
    return Unlock(self);
}

uint32_t __thiscall FileEndRequest(File* self) {
    Lock(self);
    if (--self->pending == 0 && self->waiting) {
        Engine<EventCountFn>(kEventSignalRva)(&self->event, kSignalAll);
        self->waiting = 0;
    }
    return Unlock(self);
}

bool __thiscall FileIsIdle(File* self) {
    Lock(self);
    bool idle = self->pending == 0;
    Unlock(self);
    return idle;
}

void __thiscall FileWaitPolling(File* self) {
    while (!Slot<BoolMethodFn>(self, kFileSlotIsIdle)(self)) {
        DriveCall(self, kDriveSlotService);
        Engine<SleepFn>(kSleepRva)(0);
    }
}

void __thiscall FileWaitBlocking(File* self) {
    Lock(self);
    if (static_cast<uint32_t>(self->pending) == 0) {
        Unlock(self);
        return;
    }
    if (!self->waiting) {
        if (!self->event_created) {
            Engine<EventCreateFn>(kEventCreateRva)(&self->event, 1);
            self->event_created = 1;
        } else {
            Engine<EventCountFn>(kEventResetRva)(&self->event, 1);
        }
        self->waiting = 1;
    }
    Unlock(self);
    Engine<EventFn>(kEventWaitRva)(&self->event);
}

bool __thiscall FileChangeDrive(File* self, Drive* drive) {
    Lock(self);
    if (self->drive != drive) {
        if (self->pending != 1 || self->open) {
            Unlock(self);
            return false;
        }
        DriveCall(self, kDriveSlotRelease);
        self->drive = drive;
        DriveCall(self, kDriveSlotAddRef);
    }
    Unlock(self);
    return true;
}

// ---- asynchronous operations ----------------------------------------------------------------------------------

namespace {

constexpr uint32_t kRequestD910 = 0x0064d910;
constexpr uint32_t kRequestD950 = 0x0064d950;
constexpr uint32_t kRequestD9B0 = 0x0064d9b0;
constexpr uint32_t kRequestDA10 = 0x0064da10;
constexpr uint32_t kRequestDA60 = 0x0064da60;
constexpr uint32_t kRequestDAB0 = 0x0064dab0;
constexpr uint32_t kRequestDB00 = 0x0064db00;
constexpr uint32_t kRequestD670 = 0x0064d670;
constexpr uint32_t kRequestD5E0 = 0x0064d5e0;

using CtorD670 = void*(__thiscall*)(void*, void* tls, File*, uint32_t, uint32_t);
using CtorD5E0 = void*(__thiscall*)(void*, uint32_t, Drive*);

}  // namespace

uint32_t __thiscall FileQueueD910(File* self) {
    return QueueNew(self, 0x0c, 0, [&](void* r) { return Engine<Ctor1>(kRequestD910)(r, self); });
}

uint32_t __thiscall FileQueueD950(File* self, uint32_t a, uint32_t b, uint32_t c) {
    return QueueNew(self, 0x18, 0, [&](void* r) { return Engine<Ctor4>(kRequestD950)(r, self, a, b, c); });
}

uint32_t __thiscall FileQueueD9B0(File* self, uint32_t a, uint32_t b, uint32_t c) {
    return QueueNew(self, 0x18, 0, [&](void* r) { return Engine<Ctor4>(kRequestD9B0)(r, self, a, b, c); });
}

uint32_t __thiscall FileQueueDA10(File* self, uint32_t a) {
    return QueueNew(self, 0x10, 0, [&](void* r) { return Engine<Ctor2>(kRequestDA10)(r, self, a); });
}

uint32_t __thiscall FileQueueDA60(File* self, uint32_t a) {
    return QueueNew(self, 0x10, 0, [&](void* r) { return Engine<Ctor2>(kRequestDA60)(r, self, a); });
}

uint32_t __thiscall FileQueueDAB0(File* self, uint32_t a) {
    return QueueNew(self, 0x10, 0, [&](void* r) { return Engine<Ctor2>(kRequestDAB0)(r, self, a); });
}

uint32_t __thiscall FileQueueDB00(File* self) {
    return QueueNew(self, 0x0c, 1, [&](void* r) { return Engine<Ctor1>(kRequestDB00)(r, self); });
}

uint32_t __thiscall FileQueueD670(File* self, uint32_t a, uint32_t b) {
    // The thread-local value is read only when the allocation succeeded, right before construction.
    return QueueNew(self, 0x18, 0, [&](void* r) {
        void* tls = Engine<TlsFn>(kThreadLocalRva)();
        return Engine<CtorD670>(kRequestD670)(r, tls, self, a, b);
    });
}

uint32_t __thiscall FileQueueD5E0(File* self, uint32_t a) {
    return QueueNew(self, 0x10, 0, [&](void* r) { return Engine<CtorD5E0>(kRequestD5E0)(r, a, self->drive); });
}

// ---- synchronous wrappers -------------------------------------------------------------------------------------

namespace {

uint32_t Wait(File* f) {
    return Slot<MethodFn>(f, kFileSlotWait)(f);
}

}  // namespace

uint32_t __thiscall FileSlot3AndWait(File* self, uint32_t a, uint32_t b, uint32_t c) {
    Slot<Method3Fn>(self, 3)(self, a, b, c);
    return Wait(self);
}

uint32_t __thiscall FileSlot4AndWait(File* self, uint32_t a, uint32_t b, uint32_t c) {
    Slot<Method3Fn>(self, 4)(self, a, b, c);
    return Wait(self);
}

uint32_t __thiscall FileSlot5AndWait(File* self, uint32_t a) {
    Slot<Method1Fn>(self, 5)(self, a);
    return Wait(self);
}

uint32_t __thiscall FileSlot6AndWait(File* self, uint32_t a) {
    Slot<Method1Fn>(self, 6)(self, a);
    return Wait(self);
}

uint32_t __thiscall FileSlot7AndWait(File* self, uint32_t a) {
    Slot<Method1Fn>(self, 7)(self, a);
    return Wait(self);
}

uint32_t __thiscall FileSlot8AndWait(File* self) {
    Slot<MethodFn>(self, 8)(self);
    return Wait(self);
}

// ---- accessors ------------------------------------------------------------------------------------------------

uint32_t __thiscall FileStatusLocked(File* self) {
    Lock(self);
    uint32_t status = self->status;
    Unlock(self);
    return status;
}

const char* __thiscall FileNameChecked(File* self) {
    if (Slot<ByteMethodFn>(self->drive, kDriveSlotNotify)(self->drive) < 0) {
        return nullptr;
    }
    return self->name;
}

asm(".globl _FileDriveSlot4\n"
    "_FileDriveSlot4:\n\t"
    "movl 0xc(%ecx), %ecx\n\t"   // drive
    "movl (%ecx), %eax\n\t"
    "movl 0x10(%eax), %edx\n\t"  // slot 4
    "jmp *%edx\n");

uint32_t __thiscall FileField134(File* self) {
    return self->field_134;
}

uint8_t __thiscall FileFlag16c(File* self) {
    return self->flag_16c;
}

uint32_t __thiscall FileBlockSize(File* self) {
    return self->block_size;
}

void __thiscall FileSetField12c(File* self, uint32_t v) {
    self->field_12c = v;
}

void* __thiscall FileSubObject(File* self) {
    return self->sub_138;
}

}  // namespace core
