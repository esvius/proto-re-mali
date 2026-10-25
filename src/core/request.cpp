#include "request.h"

#include "../host/hooks.h"
#include "file.h"
#include "file_system.h"

namespace core {

namespace {

// ---- engine globals ----
constexpr uint32_t kRequestMutexRva = 0x0127ca44;
constexpr uint32_t kRequestPoolRva = 0x0127ca5c;

// ---- vtables (rva) ----
constexpr uint32_t kVtRequest = 0x00e4787c;
constexpr uint32_t kVtCompletionEvent = 0x00e4789c;
constexpr uint32_t kVtFileCompletion = 0x00e478bc;
constexpr uint32_t kVtDriveCompletion = 0x00e478e0;
constexpr uint32_t kVtFileRequest = 0x00e47904;
constexpr uint32_t kVtFileOpen = 0x00e47924;
constexpr uint32_t kVtFileOpenByKey = 0x00e47944;
constexpr uint32_t kVtSaveGameOpen = 0x00e47964;
constexpr uint32_t kVtFileClose = 0x00e47984;
constexpr uint32_t kVtFileCommit = 0x00e479a4;
constexpr uint32_t kVtFileRead = 0x00e479c4;
constexpr uint32_t kVtFileWrite = 0x00e479e4;
constexpr uint32_t kVtFileSetPosition = 0x00e47a04;
constexpr uint32_t kVtFileGetPosition = 0x00e47a24;
constexpr uint32_t kVtFileGetSize = 0x00e47a44;
constexpr uint32_t kVtFileCancel = 0x00e47a64;
constexpr uint32_t kVtCompletionCallback = 0x00e47c04;

// ---- engine functions called through their live entry points ----
constexpr uint32_t kMutexLockRva = 0x0063d290;
constexpr uint32_t kMutexUnlockRva = 0x0063d280;
constexpr uint32_t kMutexDestroyRva = 0x0063d270;
constexpr uint32_t kMarkerRva = 0x006447f0;
constexpr uint32_t kRequestFreeRva = 0x0064c510;
constexpr uint32_t kRequestDestructRva = 0x0064c540;
constexpr uint32_t kRequestConstructRva = 0x0064d5a0;
constexpr uint32_t kFileRequestConstructRva = 0x0064d750;
constexpr uint32_t kFileRequestDestructRva = 0x0064c750;
constexpr uint32_t kCompletionCallbackConstructRva = 0x0064d630;
constexpr uint32_t kSetResultRva = 0x0064c780;
constexpr uint32_t kFileBeginRequestRva = 0x0064e7f0;
constexpr uint32_t kFileEndRequestRva = 0x0064e810;
constexpr uint32_t kFileChangeDriveRva = 0x0064e790;
constexpr uint32_t kEventCompleteRva = 0x0063d2d0;         // cdecl(event, 1)
constexpr uint32_t kHolderOfDriveRva = 0x0064ff10;
constexpr uint32_t kHolderBeginRva = 0x00650640;
constexpr uint32_t kHolderNextRva = 0x00650660;
constexpr uint32_t kHolderEndRva = 0x00650670;
constexpr uint32_t kLibraryOpenByNameRva = 0x00650cf0;
constexpr uint32_t kLibraryFindAndOpenRva = 0x00650ab0;
constexpr uint32_t kDriveLastErrorRva = 0x0064fb90;
constexpr uint32_t kDriveSetSearchFailedRva = 0x0064fbc0;  // thiscall(drive, 1)
constexpr uint32_t kDriveInfoRva = 0x0064fbf0;            // returns a block whose +0xc is the block size
constexpr uint32_t kDriveScheduleRva = 0x0064fc00;        // thiscall(owner, completion request)
constexpr uint32_t kDriveBeginRequestRva = 0x0064f6b0;
constexpr uint32_t kDriveEndRequestRva = 0x0064f6d0;
constexpr uint32_t kDriveCancelRva = 0x0064fec0;          // thiscall(drive, file, priority)
constexpr uint32_t kRangeIsArchiveRva = 0x00650e10;
constexpr uint32_t kRangeStreamRva = 0x00650e20;           // the archive's own File (range +0)
constexpr uint32_t kRangeSizeRva = 0x00650e40;
constexpr uint32_t kRangeCloseRva = 0x00650e50;
constexpr uint32_t kRangeReadRva = 0x006511c0;             // thiscall, 9 arguments
constexpr uint32_t kRoundUpRva = 0x00645300;

// Drive slots used by file requests.
constexpr int kDriveSlotLastErrorCode = 9;  // vtable +0x24
constexpr int kDriveSlotOpen = 10;          // +0x28 (name, mode, flag, &handle, &size)
constexpr int kDriveSlotOpenSave = 11;      // +0x2c (name, mode, flag, a, b, &handle, &size)
constexpr int kDriveSlotClose = 12;         // +0x30 (handle, name)
constexpr int kDriveSlotCommit = 13;        // +0x34 (handle, name)
constexpr int kDriveSlotRead = 14;          // +0x38
constexpr int kDriveSlotWrite = 15;         // +0x3c
constexpr int kFileSlotTail = 26;           // +0x68 (FileRequestForwardFileSlot26)

constexpr uint32_t kStatusRefused = 8;  // archive-backed open refused / write to read-only file
constexpr uint32_t kStatusNotFound = 1;

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

void SetVtable(Request* r, uint32_t rva) {
    r->vtable = protore::EngineAddress(rva);
}

using CdeclVoid = void(__cdecl*)();
using CdeclPtr = void(__cdecl*)(void*);
using MarkerFn = const void*(__cdecl*)();
using Method = uint32_t(__thiscall*)(void*);
using Method1 = uint32_t(__thiscall*)(void*, uint32_t);
using Method2 = uint32_t(__thiscall*)(void*, uint32_t, uint32_t);

void Marker() {
    Engine<MarkerFn>(kMarkerRva)();
}

uint32_t Call(void* object, int slot) {
    return Slot<Method>(object, slot)(object);
}

uint32_t ThisCall(uint32_t rva, void* self) {
    return Engine<Method>(rva)(self);
}

uint32_t ThisCall1(uint32_t rva, void* self, uint32_t a) {
    return Engine<Method1>(rva)(self, a);
}

uint32_t ThisCall2(uint32_t rva, void* self, uint32_t a, uint32_t b) {
    return Engine<Method2>(rva)(self, a, b);
}

uint32_t P(const void* p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

uint32_t SetResult(FileRequest* r, uint32_t result, uint32_t status) {
    return ThisCall2(kSetResultRva, r, result, status);
}

uint32_t LastError(Drive* drive) {
    return ThisCall(kDriveLastErrorRva, drive);
}

uint32_t BlockSize(Drive* drive) {
    const uint8_t* info = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(ThisCall(kDriveInfoRva, drive)));
    return *reinterpret_cast<const uint32_t*>(info + 0xc);
}

void* Range(File* f) {
    return f->sub_138;
}

uint32_t& Handle(File* f) {
    return *reinterpret_cast<uint32_t*>(f->unknown_164 + 4);  // +0x168
}

void* ObjectLock(void* object) {
    return static_cast<uint8_t*>(object) + 4;
}

// File found inside an archive: move it onto the archive's drive and take the archive file's handle, the range
// size and the archive's block size. Shared by open-by-name (both lists) and open-by-key.
uint32_t AdoptArchiveRange(FileRequest* self, bool refuse) {
    File* file = self->file;
    if (!refuse) {
        auto* archive = reinterpret_cast<File*>(static_cast<uintptr_t>(ThisCall(kRangeStreamRva, Range(file))));
        if (Engine<bool(__thiscall*)(File*, Drive*)>(kFileChangeDriveRva)(file, archive->drive)) {
            Handle(self->file) = Handle(archive);
            self->file->field_134 = ThisCall(kRangeSizeRva, Range(self->file));
            self->file->open = 1;
            self->file->block_size = archive->block_size;
            return SetResult(self, 1, 0);
        }
    }
    ThisCall(kRangeCloseRva, Range(self->file));
    return SetResult(self, 0, kStatusRefused);
}

// Searches the libraries of one list for the file; true and the range filled when found. The iteration is
// always ended before returning.
bool SearchLibraries(File* file, IoBufferHolder* holder, uint32_t list) {
    ThisCall1(kHolderBeginRva, holder, list);
    for (void* library = reinterpret_cast<void*>(static_cast<uintptr_t>(ThisCall(kHolderNextRva, holder))); library;
         library = reinterpret_cast<void*>(static_cast<uintptr_t>(ThisCall(kHolderNextRva, holder)))) {
        if (static_cast<uint8_t>(ThisCall2(kLibraryOpenByNameRva, library, P(file->name), P(file->sub_138)))) {
            ThisCall(kHolderEndRva, holder);
            return true;
        }
    }
    ThisCall(kHolderEndRva, holder);
    return false;
}

}  // namespace

// ---- pool ---------------------------------------------------------------------------------------------------

void* RequestAllocate(uint32_t) {
    void* mutex = protore::EngineAddress(kRequestMutexRva);
    Engine<CdeclPtr>(kMutexLockRva)(mutex);
    void* pool = Global<void*>(kRequestPoolRva);
    void* request = reinterpret_cast<void*>(static_cast<uintptr_t>(Call(pool, 18)));
    Engine<CdeclPtr>(kMutexUnlockRva)(mutex);
    return request;
}

void RequestFree(void* request) {
    void* mutex = protore::EngineAddress(kRequestMutexRva);
    Engine<CdeclPtr>(kMutexLockRva)(mutex);
    void* pool = Global<void*>(kRequestPoolRva);
    Slot<Method1>(pool, 4)(pool, P(request));
    Engine<CdeclPtr>(kMutexUnlockRva)(mutex);
}

void RequestPoolShutdown() {
    Engine<CdeclPtr>(kMutexDestroyRva)(protore::EngineAddress(kRequestMutexRva));
    uint32_t enumerated = 0;
    void* pool = Global<void*>(kRequestPoolRva);
    Slot<Method2>(pool, 11)(pool, 0, P(&enumerated));
    Call(Global<void*>(kRequestPoolRva), 1);
    Global<void*>(kRequestPoolRva) = nullptr;
}

// ---- Request / FileRequest ----------------------------------------------------------------------------------

Request* __thiscall RequestConstruct(Request* self) {
    SetVtable(self, kVtRequest);
    self->link = 0;
    Marker();
    return self;
}

void __thiscall RequestDestruct(Request* self) {
    SetVtable(self, kVtRequest);
}

Request* __thiscall RequestDeletingDestruct(Request* self, uint32_t flags) {
    ThisCall(kRequestDestructRva, self);
    if (flags & 1) {
        Engine<CdeclPtr>(kRequestFreeRva)(self);
    }
    return self;
}

FileRequest* __thiscall FileRequestConstruct(FileRequest* self, File* file) {
    ThisCall(kRequestConstructRva, self);
    SetVtable(self, kVtFileRequest);
    self->file = file;
    Marker();
    ThisCall(kFileBeginRequestRva, self->file);
    Call(self->file, 0);
    return self;
}

void __thiscall FileRequestDestruct(FileRequest* self) {
    SetVtable(self, kVtFileRequest);
    ThisCall(kFileEndRequestRva, self->file);
    Call(self->file, 1);
    ThisCall(kRequestDestructRva, self);
}

Request* __thiscall FileRequestDeletingDestruct(FileRequest* self, uint32_t flags) {
    ThisCall(kFileRequestDestructRva, self);
    if (flags & 1) {
        Engine<CdeclPtr>(kRequestFreeRva)(self);
    }
    return self;
}

uint32_t __thiscall FileRequestSetResult(FileRequest* self, uint32_t result, uint32_t status) {
    Call(ObjectLock(self->file), 0);
    self->file->status = status;
    File* file = self->file;
    Call(ObjectLock(file), 1);
    return result;
}

uint32_t __thiscall FileRequestIsForFile(FileRequest* self, File* file) {
    return file == self->file ? 1 : 0;
}

Drive* __thiscall FileRequestDrive(FileRequest* self) {
    return self->file->drive;
}

uint32_t __thiscall FileRequestFailFromDrive(FileRequest* self) {
    Drive* drive = self->file->drive;
    uint32_t code = Call(drive, kDriveSlotLastErrorCode);
    uint32_t status = LastError(drive);
    return SetResult(self, code, status);
}

asm(".globl _FileRequestForwardFileSlot26\n"
    "_FileRequestForwardFileSlot26:\n\t"
    "movl 0x8(%ecx), %ecx\n\t"
    "movl (%ecx), %eax\n\t"
    "movl 0x68(%eax), %edx\n\t"
    "jmp *%edx\n");

// ---- completion requests ------------------------------------------------------------------------------------

CompletionRequest* __thiscall CompletionEventConstruct(CompletionRequest* self, uint32_t event, void* object) {
    ThisCall(kRequestConstructRva, self);
    SetVtable(self, kVtCompletionEvent);
    self->target = event;
    self->object = object;
    Marker();
    Call(self->object, 0);
    return self;
}

void __thiscall CompletionEventDestruct(CompletionRequest* self) {
    SetVtable(self, kVtCompletionEvent);
    Call(self->object, 1);
    ThisCall(kRequestDestructRva, self);
}

uint32_t __thiscall CompletionEventExecute(CompletionRequest* self) {
    Engine<void(__cdecl*)(uint32_t, uint32_t)>(kEventCompleteRva)(self->target, 1);
    return 1;
}

CompletionRequest* __thiscall CompletionCallbackConstruct(CompletionRequest* self, uint32_t target) {
    ThisCall(kRequestConstructRva, self);
    SetVtable(self, kVtCompletionCallback);
    self->target = target;
    Marker();
    return self;
}

uint32_t __thiscall CompletionCallbackIsFor(CompletionRequest* self, uint32_t target) {
    return target == self->target ? 1 : 0;
}

namespace {

CallbackCompletionRequest* ConstructCallbackCompletion(CallbackCompletionRequest* self, uint32_t target,
                                                       void* owner, void* callback, uint32_t argument,
                                                       uint32_t vtable, uint32_t owner_begin_rva) {
    ThisCall1(kCompletionCallbackConstructRva, self, target);
    SetVtable(self, vtable);
    self->object = owner;
    self->callback = callback;
    self->argument = argument;
    Marker();
    Call(self->callback, 0);
    ThisCall(owner_begin_rva, self->object);
    Call(self->object, 0);
    return self;
}

void DestructCallbackCompletion(CallbackCompletionRequest* self, uint32_t vtable) {
    SetVtable(self, vtable);
    Call(self->callback, 1);
    Call(self->object, 1);
    ThisCall(kRequestDestructRva, self);
}

// Completion requests are scheduled on the object found at owner + 0xc (the file's drive, or the drive's own
// owner for drive completions).
uint32_t ScheduleOn(CallbackCompletionRequest* self) {
    void* owner = *reinterpret_cast<void**>(static_cast<uint8_t*>(self->object) + 0xc);
    ThisCall1(kDriveScheduleRva, owner, P(self));
    return 3;
}

uint32_t Finish(CallbackCompletionRequest* self, uint32_t owner_end_rva) {
    ThisCall(owner_end_rva, self->object);
    return Slot<Method1>(self->callback, 3)(self->callback, self->argument);
}

}  // namespace

CallbackCompletionRequest* __thiscall FileCompletionConstruct(CallbackCompletionRequest* self, uint32_t target,
                                                              File* file, void* callback, uint32_t argument) {
    return ConstructCallbackCompletion(self, target, file, callback, argument, kVtFileCompletion,
                                       kFileBeginRequestRva);
}

void __thiscall FileCompletionDestruct(CallbackCompletionRequest* self) {
    DestructCallbackCompletion(self, kVtFileCompletion);
}

uint32_t __thiscall FileCompletionExecute(CallbackCompletionRequest* self) {
    return ScheduleOn(self);
}

uint32_t __thiscall FileCompletionFinish(CallbackCompletionRequest* self) {
    return Finish(self, kFileEndRequestRva);
}

CallbackCompletionRequest* __thiscall DriveCompletionConstruct(CallbackCompletionRequest* self, uint32_t target,
                                                               Drive* drive, void* callback, uint32_t argument) {
    return ConstructCallbackCompletion(self, target, drive, callback, argument, kVtDriveCompletion,
                                       kDriveBeginRequestRva);
}

void __thiscall DriveCompletionDestruct(CallbackCompletionRequest* self) {
    DestructCallbackCompletion(self, kVtDriveCompletion);
}

uint32_t __thiscall DriveCompletionExecute(CallbackCompletionRequest* self) {
    return ScheduleOn(self);
}

uint32_t __thiscall DriveCompletionFinish(CallbackCompletionRequest* self) {
    return Finish(self, kDriveEndRequestRva);
}

namespace {

template <uint32_t DestructRva>
Request* DeletingVia(Request* self, uint32_t flags) {
    ThisCall(DestructRva, self);
    if (flags & 1) {
        Engine<CdeclPtr>(kRequestFreeRva)(self);
    }
    return self;
}

}  // namespace

Request* __thiscall CompletionEventDeleting(CompletionRequest* self, uint32_t flags) {
    return DeletingVia<0x0064c550>(self, flags);
}

Request* __thiscall CompletionCallbackDeleting(CompletionRequest* self, uint32_t flags) {
    return DeletingVia<kRequestDestructRva>(self, flags);
}

Request* __thiscall FileCompletionDeleting(CallbackCompletionRequest* self, uint32_t flags) {
    return DeletingVia<0x0064c630>(self, flags);
}

Request* __thiscall DriveCompletionDeleting(CallbackCompletionRequest* self, uint32_t flags) {
    return DeletingVia<0x0064c6c0>(self, flags);
}

// ---- file requests: constructors ------------------------------------------------------------------------------

namespace {

template <typename T>
T* BaseConstruct(T* self, File* file, uint32_t vtable) {
    ThisCall1(kFileRequestConstructRva, self, P(file));
    SetVtable(self, vtable);
    return self;
}

}  // namespace

FileOpenRequest* __thiscall FileOpenConstruct(FileOpenRequest* self, File* file, uint32_t mode, uint32_t flag,
                                              uint32_t search) {
    BaseConstruct(self, file, kVtFileOpen);
    self->mode = mode;
    self->flag = static_cast<uint8_t>(flag);
    self->search = search;
    Marker();
    return self;
}

FileOpenByKeyRequest* __thiscall FileOpenByKeyConstruct(FileOpenByKeyRequest* self, File* file, uint32_t key,
                                                        void* library) {
    BaseConstruct(self, file, kVtFileOpenByKey);
    self->key = key;
    self->library = library;
    Marker();
    Call(self->library, 0);
    return self;
}

SaveGameOpenRequest* __thiscall SaveGameOpenConstruct(SaveGameOpenRequest* self, File* file, uint32_t mode,
                                                      uint32_t flag, uint32_t a, uint32_t b) {
    ThisCall1(kFileRequestConstructRva, self, P(file));
    self->mode = mode;
    SetVtable(self, kVtSaveGameOpen);
    self->flag = static_cast<uint8_t>(flag);
    self->a = a;
    self->b = b;
    Marker();
    return self;
}

FileRequest* __thiscall FileCloseConstruct(FileRequest* self, File* file) {
    BaseConstruct(self, file, kVtFileClose);
    Marker();
    return self;
}

FileRequest* __thiscall FileCommitConstruct(FileRequest* self, File* file) {
    BaseConstruct(self, file, kVtFileCommit);
    Marker();
    return self;
}

namespace {

FileTransferRequest* TransferConstruct(FileTransferRequest* self, File* file, void* buffer, uint32_t count,
                                       uint32_t flags, uint32_t vtable) {
    BaseConstruct(self, file, vtable);
    self->buffer = buffer;
    self->count = count;
    self->flags = flags;
    Marker();
    return self;
}

FileValueRequest* ValueConstruct(FileValueRequest* self, File* file, uint32_t value, uint32_t vtable) {
    BaseConstruct(self, file, vtable);
    self->value = value;
    Marker();
    return self;
}

}  // namespace

FileTransferRequest* __thiscall FileReadConstruct(FileTransferRequest* self, File* file, void* buffer,
                                                  uint32_t count, uint32_t flags) {
    return TransferConstruct(self, file, buffer, count, flags, kVtFileRead);
}

FileTransferRequest* __thiscall FileWriteConstruct(FileTransferRequest* self, File* file, void* buffer,
                                                   uint32_t count, uint32_t flags) {
    return TransferConstruct(self, file, buffer, count, flags, kVtFileWrite);
}

FileValueRequest* __thiscall FileSetPositionConstruct(FileValueRequest* self, File* file, uint32_t position) {
    return ValueConstruct(self, file, position, kVtFileSetPosition);
}

FileValueRequest* __thiscall FileGetPositionConstruct(FileValueRequest* self, File* file, uint32_t out) {
    return ValueConstruct(self, file, out, kVtFileGetPosition);
}

FileValueRequest* __thiscall FileGetSizeConstruct(FileValueRequest* self, File* file, uint32_t out) {
    return ValueConstruct(self, file, out, kVtFileGetSize);
}

FileRequest* __thiscall FileCancelConstruct(FileRequest* self, File* file) {
    BaseConstruct(self, file, kVtFileCancel);
    Marker();
    return self;
}

// ---- file requests: destructors -------------------------------------------------------------------------------

namespace {

void DestructAs(FileRequest* self, uint32_t vtable) {
    SetVtable(self, vtable);
    ThisCall(kFileRequestDestructRva, self);
}

}  // namespace

void __thiscall FileOpenDestruct(FileRequest* self) { DestructAs(self, kVtFileOpen); }
void __thiscall SaveGameOpenDestruct(FileRequest* self) { DestructAs(self, kVtSaveGameOpen); }
void __thiscall FileCloseDestruct(FileRequest* self) { DestructAs(self, kVtFileClose); }
void __thiscall FileCommitDestruct(FileRequest* self) { DestructAs(self, kVtFileCommit); }
void __thiscall FileReadDestruct(FileRequest* self) { DestructAs(self, kVtFileRead); }
void __thiscall FileWriteDestruct(FileRequest* self) { DestructAs(self, kVtFileWrite); }
void __thiscall FileSetPositionDestruct(FileRequest* self) { DestructAs(self, kVtFileSetPosition); }
void __thiscall FileGetPositionDestruct(FileRequest* self) { DestructAs(self, kVtFileGetPosition); }
void __thiscall FileGetSizeDestruct(FileRequest* self) { DestructAs(self, kVtFileGetSize); }
void __thiscall FileCancelDestruct(FileRequest* self) { DestructAs(self, kVtFileCancel); }

void __thiscall FileOpenByKeyDestruct(FileRequest* self) {
    SetVtable(self, kVtFileOpenByKey);
    Call(static_cast<FileOpenByKeyRequest*>(self)->library, 1);
    ThisCall(kFileRequestDestructRva, self);
}

// ---- file requests: execute -----------------------------------------------------------------------------------

uint32_t __thiscall FileOpenExecute(FileOpenRequest* self) {
    Drive* drive = self->file->drive;
    auto* holder = reinterpret_cast<IoBufferHolder*>(static_cast<uintptr_t>(ThisCall(kHolderOfDriveRva, drive)));
    if (SearchLibraries(self->file, holder, 0)) {
        return AdoptArchiveRange(self, self->file->flag_27 || self->mode == 3);
    }
    uint32_t result = 0;
    if (self->search == 1) {
        ThisCall1(kDriveSetSearchFailedRva, drive, 1);
    } else {
        File* file = self->file;
        using OpenFn = uint32_t(__thiscall*)(Drive*, const char*, uint32_t, uint32_t, uint32_t*, uint32_t*);
        result = Slot<OpenFn>(drive, kDriveSlotOpen)(drive, file->name, self->mode, self->flag, &Handle(file),
                                                     &file->field_134);
        if (result == 1) {
            self->file->open = 1;
            self->file->block_size = BlockSize(drive);
            return SetResult(self, result, LastError(drive));
        }
    }
    if (SearchLibraries(self->file, holder, 1)) {
        return AdoptArchiveRange(self, self->file->flag_27 || self->mode == 3);
    }
    return SetResult(self, result, LastError(drive));
}

uint32_t __thiscall FileOpenByKeyExecute(FileOpenByKeyRequest* self) {
    Drive* drive = self->file->drive;
    if (static_cast<uint8_t>(ThisCall2(kLibraryFindAndOpenRva, self->library, self->key, P(self->file->sub_138)))) {
        return AdoptArchiveRange(self, self->file->flag_27 != 0);
    }
    ThisCall1(kDriveSetSearchFailedRva, drive, 1);
    return SetResult(self, 0, kStatusNotFound);
}

uint32_t __thiscall SaveGameOpenExecute(SaveGameOpenRequest* self) {
    File* file = self->file;
    Drive* drive = file->drive;
    using OpenSaveFn = uint32_t(__thiscall*)(Drive*, const char*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*,
                                             uint32_t*);
    uint32_t result = Slot<OpenSaveFn>(drive, kDriveSlotOpenSave)(drive, file->name, self->mode, self->flag, self->a,
                                                                  self->b, &Handle(file), &file->field_134);
    if (result == 1) {
        self->file->open = 1;
        self->file->block_size = BlockSize(drive);
    }
    return SetResult(self, result, LastError(drive));
}

uint32_t __thiscall FileCloseExecute(FileRequest* self) {
    Drive* drive = self->file->drive;
    if (!static_cast<uint8_t>(ThisCall(kRangeIsArchiveRva, Range(self->file)))) {
        File* file = self->file;
        Slot<Method2>(drive, kDriveSlotClose)(drive, Handle(file), P(file->name));
    } else {
        ThisCall(kRangeCloseRva, Range(self->file));
    }
    self->file->open = 0;
    return SetResult(self, 1, 0);
}

uint32_t __thiscall FileCommitExecute(FileRequest* self) {
    File* file = self->file;
    Drive* drive = file->drive;
    uint32_t result = Slot<Method2>(drive, kDriveSlotCommit)(drive, Handle(file), P(file->name));
    return SetResult(self, result, LastError(drive));
}

uint32_t __thiscall FileReadExecute(FileTransferRequest* self) {
    File* file = self->file;
    const uint32_t size = file->field_134;
    Drive* drive = file->drive;
    uint32_t count = self->count;
    if (file->field_130 >= size) {
        count = 0;
    }
    const uint32_t limit = Engine<uint32_t(__cdecl*)(uint32_t, uint32_t)>(kRoundUpRva)(size, BlockSize(drive));
    const uint32_t position = self->file->field_130;
    if (position + count > limit) {
        count = limit - position;
    }
    // The original keeps "bytes transferred" in the stack slot that saved ecx (this request).
    uint32_t transferred = P(self);
    uint32_t result;
    if (count) {
        file = self->file;
        if (static_cast<uint8_t>(ThisCall(kRangeIsArchiveRva, Range(file)))) {
            using RangeReadFn = uint32_t(__thiscall*)(void*, Drive*, uint32_t, const char*, uint32_t, uint32_t, void*,
                                                      uint32_t, uint32_t*, uint32_t);
            result = Engine<RangeReadFn>(kRangeReadRva)(Range(file), drive, Handle(file), file->name, file->field_12c,
                                                        file->field_130, self->buffer, count, &transferred,
                                                        self->flags);
        } else {
            using ReadFn = uint32_t(__thiscall*)(Drive*, uint32_t, const char*, uint32_t, uint32_t, void*, uint32_t,
                                                 uint32_t*, uint32_t);
            result = Slot<ReadFn>(drive, kDriveSlotRead)(drive, Handle(file), file->name, file->field_12c,
                                                         file->field_130, self->buffer, count, &transferred,
                                                         self->flags);
        }
        if (result == 1) {
            self->file->field_130 += transferred;
        }
    } else {
        result = 1;
    }
    return SetResult(self, result, LastError(drive));
}

uint32_t __thiscall FileWriteExecute(FileTransferRequest* self) {
    File* file = self->file;
    if (!file->flag_27) {
        return SetResult(self, 0, kStatusRefused);
    }
    Drive* drive = file->drive;
    uint32_t transferred = P(self);
    uint32_t result;
    if (self->count) {
        using WriteFn = uint32_t(__thiscall*)(Drive*, uint32_t, const char*, uint32_t, uint32_t, void*, uint32_t,
                                              uint32_t*, uint32_t*, uint32_t);
        result = Slot<WriteFn>(drive, kDriveSlotWrite)(drive, Handle(file), file->name, file->field_12c,
                                                       file->field_130, self->buffer, self->count, &transferred,
                                                       &file->field_134, self->flags);
        if (result == 1) {
            self->file->field_130 += transferred;
        }
    } else {
        result = 1;
    }
    return SetResult(self, result, LastError(drive));
}

uint32_t __thiscall FileSetPositionExecute(FileValueRequest* self) {
    File* file = self->file;
    if (!file->flag_27 && self->value > file->field_134) {
        self->value = file->field_134;
    }
    file->field_130 = self->value;
    return SetResult(self, 1, 0);
}

uint32_t __thiscall FileGetPositionExecute(FileValueRequest* self) {
    *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(self->value)) = self->file->field_130;
    return SetResult(self, 1, 0);
}

uint32_t __thiscall FileGetSizeExecute(FileValueRequest* self) {
    *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(self->value)) = self->file->field_134;
    return SetResult(self, 1, 0);
}

uint32_t __thiscall FileCancelExecute(FileRequest* self) {
    File* file = self->file;
    ThisCall2(kDriveCancelRva, file->drive, P(file), file->priority);
    return SetResult(self, 1, 0);
}

}  // namespace core
