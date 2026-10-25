// core requests (code\core\src\file): every file system operation is a Request object from a dedicated pool,
// queued on a drive and executed by its thread through vtable slot 2. Class layouts and behaviour observed from the
// engine; all objects are engine-owned, so they are described by explicit layouts and their vtables are the
// engine's own (written by the reconstructed constructors).
//
// Vtable slots common to all requests: 1 deleting destructor, 2 execute, 7 type code. Execute returns a result
// code; file requests publish their status through FileRequestSetResult, which stores it in File::status.
#pragma once

#include <cstdint>

namespace core {

struct File;
struct Drive;

struct Request {
    void* vtable;   // +0x00
    uint32_t link;  // +0x04 zeroed by the constructor
};

struct FileRequest : Request {
    File* file;  // +0x08 the file keeps one pending count and one reference per live request
};

struct FileOpenRequest : FileRequest {
    uint32_t mode;    // +0x0c passed to the drive; 3 = never take the file from an archive
    uint8_t flag;     // +0x10 passed to the drive
    uint8_t pad[3];
    uint32_t search;  // +0x14 1 = archives only (second list), no file system lookup
};

struct FileOpenByKeyRequest : FileRequest {
    uint32_t key;     // +0x0c
    void* library;    // +0x10 CementLibrary (referenced)
};

struct SaveGameOpenRequest : FileRequest {
    uint32_t mode;    // +0x0c
    uint8_t flag;     // +0x10
    uint8_t pad[3];
    uint32_t a;       // +0x14
    uint32_t b;       // +0x18
};

struct FileTransferRequest : FileRequest {  // read and write
    void* buffer;     // +0x0c
    uint32_t count;   // +0x10
    uint32_t flags;   // +0x14
};

struct FileValueRequest : FileRequest {  // set position (value), get position / get size (out pointer)
    uint32_t value;   // +0x0c
};

struct CompletionRequest : Request {  // CompletionEventRequest / CompletionCallbackRequest base fields
    uint32_t target;  // +0x08 event (CompletionEvent) or owner key (CompletionCallback)
    void* object;     // +0x0c CompletionEvent: referenced object; File/DriveCompletion: file or drive
};

struct CallbackCompletionRequest : CompletionRequest {  // FileCompletionRequest / DriveCompletionRequest
    void* callback;     // +0x10 referenced; its slot 3 receives `argument` when the request finishes
    uint32_t argument;  // +0x14
};

// ---- pool (mutex at rva 0x0127ca44, pool object at 0x0127ca5c) ----
void* RequestAllocate(uint32_t ignored_size);  // FUN_1064c4e0
void RequestFree(void* request);               // FUN_1064c510
void RequestPoolShutdown();                    // FUN_1064d550

// ---- Request / FileRequest ----
Request* __thiscall RequestConstruct(Request* self);                                  // FUN_1064d5a0
void __thiscall RequestDestruct(Request* self);                                        // FUN_1064c540
Request* __thiscall RequestDeletingDestruct(Request* self, uint32_t flags);           // FUN_1064d5c0
FileRequest* __thiscall FileRequestConstruct(FileRequest* self, File* file);          // FUN_1064d750
void __thiscall FileRequestDestruct(FileRequest* self);                                // FUN_1064c750
Request* __thiscall FileRequestDeletingDestruct(FileRequest* self, uint32_t flags);   // FUN_1064d790
uint32_t __thiscall FileRequestSetResult(FileRequest* self, uint32_t result, uint32_t status);  // FUN_1064c780
uint32_t __thiscall FileRequestIsForFile(FileRequest* self, File* file);              // FUN_1064c7b0
Drive* __thiscall FileRequestDrive(FileRequest* self);                                // FUN_1064c7c0
uint32_t __thiscall FileRequestFailFromDrive(FileRequest* self);                      // FUN_1064c7d0
extern "C" void FileRequestForwardFileSlot26();                                        // FUN_1064c800 (asm)

// ---- completion requests ----
CompletionRequest* __thiscall CompletionEventConstruct(CompletionRequest* self, uint32_t event, void* object);  // d5e0
void __thiscall CompletionEventDestruct(CompletionRequest* self);                      // FUN_1064c550
uint32_t __thiscall CompletionEventExecute(CompletionRequest* self);                   // FUN_1064c570
CompletionRequest* __thiscall CompletionCallbackConstruct(CompletionRequest* self, uint32_t target);  // d630
uint32_t __thiscall CompletionCallbackIsFor(CompletionRequest* self, uint32_t target);  // FUN_1064c600
CallbackCompletionRequest* __thiscall FileCompletionConstruct(CallbackCompletionRequest* self, uint32_t target,
                                                              File* file, void* callback, uint32_t argument);  // d670
void __thiscall FileCompletionDestruct(CallbackCompletionRequest* self);               // FUN_1064c630
uint32_t __thiscall FileCompletionExecute(CallbackCompletionRequest* self);            // FUN_1064c660
uint32_t __thiscall FileCompletionFinish(CallbackCompletionRequest* self);             // FUN_1064c680
CallbackCompletionRequest* __thiscall DriveCompletionConstruct(CallbackCompletionRequest* self, uint32_t target,
                                                               Drive* drive, void* callback, uint32_t argument);  // d6e0
void __thiscall DriveCompletionDestruct(CallbackCompletionRequest* self);              // FUN_1064c6c0
uint32_t __thiscall DriveCompletionExecute(CallbackCompletionRequest* self);           // FUN_1064c6f0
uint32_t __thiscall DriveCompletionFinish(CallbackCompletionRequest* self);            // FUN_1064c710

// ---- file requests: constructors ----
FileOpenRequest* __thiscall FileOpenConstruct(FileOpenRequest* self, File* file, uint32_t mode, uint32_t flag,
                                              uint32_t search);                                        // d7b0
FileOpenByKeyRequest* __thiscall FileOpenByKeyConstruct(FileOpenByKeyRequest* self, File* file, uint32_t key,
                                                        void* library);                                // d810
SaveGameOpenRequest* __thiscall SaveGameOpenConstruct(SaveGameOpenRequest* self, File* file, uint32_t mode,
                                                      uint32_t flag, uint32_t a, uint32_t b);           // d870
FileRequest* __thiscall FileCloseConstruct(FileRequest* self, File* file);                              // d8d0
FileRequest* __thiscall FileCommitConstruct(FileRequest* self, File* file);                             // d910
FileTransferRequest* __thiscall FileReadConstruct(FileTransferRequest* self, File* file, void* buffer,
                                                  uint32_t count, uint32_t flags);                      // d950
FileTransferRequest* __thiscall FileWriteConstruct(FileTransferRequest* self, File* file, void* buffer,
                                                   uint32_t count, uint32_t flags);                     // d9b0
FileValueRequest* __thiscall FileSetPositionConstruct(FileValueRequest* self, File* file, uint32_t position);  // da10
FileValueRequest* __thiscall FileGetPositionConstruct(FileValueRequest* self, File* file, uint32_t out);       // da60
FileValueRequest* __thiscall FileGetSizeConstruct(FileValueRequest* self, File* file, uint32_t out);           // dab0
FileRequest* __thiscall FileCancelConstruct(FileRequest* self, File* file);                             // db00

// ---- file requests: destructors (vtable, then the parent destructor) ----
void __thiscall FileOpenDestruct(FileRequest* self);          // c810
void __thiscall FileOpenByKeyDestruct(FileRequest* self);     // cac0 (releases the library)
void __thiscall SaveGameOpenDestruct(FileRequest* self);      // cbc0
void __thiscall FileCloseDestruct(FileRequest* self);         // cc50
void __thiscall FileCommitDestruct(FileRequest* self);        // cce0
void __thiscall FileReadDestruct(FileRequest* self);          // cd40
void __thiscall FileWriteDestruct(FileRequest* self);         // ce60
void __thiscall FileSetPositionDestruct(FileRequest* self);   // cf10
void __thiscall FileGetPositionDestruct(FileRequest* self);   // cf60
void __thiscall FileGetSizeDestruct(FileRequest* self);       // cfa0
void __thiscall FileCancelDestruct(FileRequest* self);        // cfe0

// Deleting destructors share one shape: destruct, then RequestFree when flag bit 0 is set.
template <void(__thiscall* Destruct)(FileRequest*)>
Request* __thiscall FileRequestDeleting(FileRequest* self, uint32_t flags);
Request* __thiscall CompletionEventDeleting(CompletionRequest* self, uint32_t flags);            // d610
Request* __thiscall CompletionCallbackDeleting(CompletionRequest* self, uint32_t flags);         // d650
Request* __thiscall FileCompletionDeleting(CallbackCompletionRequest* self, uint32_t flags);     // d6c0
Request* __thiscall DriveCompletionDeleting(CallbackCompletionRequest* self, uint32_t flags);    // d730

// ---- file requests: execute (vtable slot 2) ----
uint32_t __thiscall FileOpenExecute(FileOpenRequest* self);            // c820
uint32_t __thiscall FileOpenByKeyExecute(FileOpenByKeyRequest* self);  // cae0
uint32_t __thiscall SaveGameOpenExecute(SaveGameOpenRequest* self);    // cbd0
uint32_t __thiscall FileCloseExecute(FileRequest* self);               // cc70
uint32_t __thiscall FileCommitExecute(FileRequest* self);              // ccf0
uint32_t __thiscall FileReadExecute(FileTransferRequest* self);        // cd50
uint32_t __thiscall FileWriteExecute(FileTransferRequest* self);       // ce70
uint32_t __thiscall FileSetPositionExecute(FileValueRequest* self);    // cf20
uint32_t __thiscall FileGetPositionExecute(FileValueRequest* self);    // cf70
uint32_t __thiscall FileGetSizeExecute(FileValueRequest* self);        // cfb0
uint32_t __thiscall FileCancelExecute(FileRequest* self);              // cff0

// ---- constant slots (each returns a fixed value; 5-6 bytes, hookable) ----
template <uint32_t Value>
uint32_t __thiscall RequestReturn(Request* self);
template <uint32_t Value>
uint32_t __thiscall RequestReturnByteWithArg(Request* self, uint32_t ignored);  // xor al,al / ret 4

}  // namespace core

// ---- template definitions ----
namespace core {

template <void(__thiscall* Destruct)(FileRequest*)>
Request* __thiscall FileRequestDeleting(FileRequest* self, uint32_t flags) {
    Destruct(self);
    if (flags & 1) {
        RequestFree(self);
    }
    return self;
}

template <uint32_t Value>
uint32_t __thiscall RequestReturn(Request*) {
    return Value;
}

template <uint32_t Value>
uint32_t __thiscall RequestReturnByteWithArg(Request*, uint32_t) {
    return Value;
}

}  // namespace core
