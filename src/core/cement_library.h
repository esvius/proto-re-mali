// core::CementLibrary — an .rcf archive mounted as a drive (code\core\src\file\common\cementer.cpp).
// Reconstructed so far: directory lookup and open-by-name. The object is still allocated and laid out by the
// original engine, so it is accessed through field offsets observed in the disassembly.
#pragma once

#include <cstdint>

namespace core {

struct CementLibrary;      // opaque: engine object, fields read via offsets in cement_library.cpp
struct CementOpenRequest;  // opaque: engine request object that receives the file's byte range

// Directory entry formats; the library's "compact" flag (+0x49) selects one.
struct CementEntryCompact {
    uint32_t key;     // core::HashFileName of the path; directory is sorted by key (unsigned)
    uint32_t offset;  // byte offset of the file inside the .rcf
    uint32_t size;
};
static_assert(sizeof(CementEntryCompact) == 12, "compact entry is 12 bytes");

struct CementEntryWide {
    uint32_t key;
    uint32_t offset;
    uint32_t size;
    uint32_t value_c;   // passed to the request in place of `size` (meaning not yet known)
    uint32_t value_10;  // passed to the request as an extra word, 0 for compact entries
};
static_assert(sizeof(CementEntryWide) == 20, "wide entry is 20 bytes");

// Load states stored at +0x24 (observed: FindAndOpen requires 1; the loader writes 1 and 2).
enum CementLibraryState : int32_t {
    kCementStateLoaded = 1,
    kCementStateFailed = 2,
};

// Original: FUN_106503c0 (rva 0x006503c0), thiscall, no stack arguments. Validates the 0x3c-byte .rcf header that
// the loader reads to +0x28: the 24-byte magic "ATG CORE CEMENT LIBRARY\0", version bytes at +0x48..+0x4b
// (major must be 2, +0x4b nonzero, minor at +0x49 below 2, +0x4a zero) and, for minor 0 (wide entries), that the
// words at +0x18 and +0x1c are equal. Every check is evaluated; the result is their conjunction (in al).
bool __thiscall CementLibraryValidateHeader(CementLibrary* self);

// Original: FUN_10650410 (rva 0x00650410), thiscall, ret 4. Stores the state; if a completion object is pending at
// +0x70, calls its slot 3 with the word at +0x74, then its slot 1, and clears both fields.
void __thiscall CementLibrarySetState(CementLibrary* self, int32_t state);

// Phases of the asynchronous load. The stream calls the library back (through its interface at +4) with the tag
// given when the read was queued.
enum CementLoadPhase : int32_t {
    kCementPhaseStart = 0,      // queue the header read
    kCementPhaseHeader = 1,     // header arrived: validate, allocate and queue the directory read
    kCementPhaseDirectory = 2,  // directory arrived: acquire the drive's I/O buffer, register, mark loaded
};

// Original: CementLibrary vfunction4 at FUN_106508e0 (rva 0x006508e0), thiscall on the library's interface
// sub-object (library + 4), ret 4. One step of the load state machine; see cement_library.cpp for the phases.
void __thiscall CementLibraryLoadStep(void* load_interface, int32_t phase);

// ---- object lifetime ----

// Optional pre-loaded directory handed to the constructor: the header copy, the entries, and their heap.
struct CementPreloaded {
    const uint8_t* header;  // 0x3c bytes
    void* entries;
    uint32_t heap;
};

// Original: FUN_10650ba0 (rva 0x00650ba0), thiscall, ret 14h. Takes references on both drives and the stream and
// queues the first load phase on the stream (phase 0, or phase 2 directly when `preloaded` is given).
CementLibrary* __thiscall CementLibraryConstruct(CementLibrary* self, void* drive, void* stream, uint32_t list_index,
                                                 const CementPreloaded* preloaded, void* registration_drive);

// Original: FUN_10650730 (rva 0x00650730), thiscall. Cancels a pending load or unregisters a loaded library (and
// releases the drive's I/O buffer when it held one), releases the drives and stream, frees the directory.
void __thiscall CementLibraryDestruct(CementLibrary* self);

// Original: FUN_10650cb0 (rva 0x00650cb0), thiscall, ret 4. Destructs; with flag bit 0 also frees the object to the
// heap the destructor published in the engine's current-heap global.
CementLibrary* __thiscall CementLibraryDeletingDestruct(CementLibrary* self, uint32_t flags);

// Reference counting on the SafeRefCount sub-object (+0x10 lock object, +0x14 count). FUN_10650830 / FUN_10650850.
uint32_t __thiscall CementLibraryAddRef(CementLibrary* self);
uint32_t __thiscall CementLibraryRelease(CementLibrary* self);

// ---- small methods ----

// FUN_10650360: polls the file system (FUN_1064a030) and yields until the state leaves 0.
void __thiscall CementLibraryWaitUntilSettled(CementLibrary* self);
// FUN_10650390: 1 when loaded, else 0 (full eax).
uint32_t __thiscall CementLibraryIsLoaded(CementLibrary* self);
// FUN_106503a0 (ret 8): remembers a completion object and its argument and takes a reference (its slot 0).
void __thiscall CementLibrarySetCompletion(CementLibrary* self, void* completion, uint32_t argument);

// FUN_10650330 / FUN_10650340: tail-jump into slot 0 / slot 1 of the interface at (*(self + 0x10)) + 4, leaving the
// caller's stack untouched. Implemented in assembly to keep that exact shape.
extern "C" void CementLibraryForwardSlot0();
extern "C" void CementLibraryForwardSlot1();

// ---- open by key ----

struct File;

// Original: FUN_10650460 (rva 0x00650460), thiscall, ret 18h. Creates a core::File named "0x<KEY>" on the library's
// drive and queues a FileOpenByKeyRequest for it on that drive. *out receives the file (or null).
void __thiscall CementLibraryOpenByKey(CementLibrary* self, File** out, uint32_t key, uint32_t a3, uint32_t a4,
                                       uint32_t a5, uint32_t a6);

// Original: FUN_10650500 (rva 0x00650500), thiscall, ret 18h. Calls the object's vtable slot 7 (OpenByKey) with the
// same arguments, then slot 21 of the resulting file when there is one.
void __thiscall CementLibraryOpenByKeyAndWait(CementLibrary* self, File** out, uint32_t key, uint32_t a3, uint32_t a4,
                                              uint32_t a5, uint32_t a6);

// Original: FUN_106502c0 (rva 0x006502c0), cdecl. Binary search over `count` records of `stride` bytes whose
// first word is an unsigned key; returns the matching record or null. The midpoint is computed as
// (hi >> 1) + (lo >> 1) + (lo & hi & 1) and the probe order must be kept: with duplicate keys it decides which
// record is returned.
const void* CementFindEntry(const void* entries, uint32_t count, uint32_t key, uint32_t stride);

// Original: FUN_10650ab0 (rva 0x00650ab0), thiscall, ret 8. If the directory is loaded (state 1), finds `key` and
// hands the file's byte range to `request` (FUN_10651130); returns true when found.
bool __thiscall CementLibraryFindAndOpen(CementLibrary* self, uint32_t key, CementOpenRequest* request);

// Original: FUN_10650cf0 (rva 0x00650cf0), thiscall, ret 8. Drops one leading '\' (HashFileName drops another),
// hashes the path and calls CementLibraryFindAndOpen. Returns its result (the original leaves it in al).
bool __thiscall CementLibraryOpenByName(CementLibrary* self, const char* path, CementOpenRequest* request);

}  // namespace core
