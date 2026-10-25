// Byte range of a file inside an .rcf archive, filled in by CementLibraryFindAndOpen when a lookup succeeds.
// Archives can be mounted inside other archives, so the absolute offset adds the base offset of every enclosing
// drive. The object itself belongs to the engine (an open request); layout observed from FUN_10651130.
#pragma once

#include <cstdint>

namespace core {

struct Drive;  // opaque engine drive; only its virtual table is used here

// Returned by Drive virtual slot 32 (vtable +0x80): where this drive sits inside its parent.
struct DriveMount {
    Drive* parent;         // null at the outermost drive
    uint32_t base_offset;  // offset of this drive's data inside the parent
};

struct CementFileRange {
    Drive* drive;            // +0x00
    uint32_t offset;         // +0x04 absolute offset (32-bit sum, no carry into offset_high)
    uint32_t offset_high;    // +0x08 always 0
    uint32_t size;           // +0x0c size argument (entry size, or value_c for wide entries)
    uint32_t size_high;      // +0x10 always 0
    uint32_t position;       // +0x14 always 0
    uint32_t entry_size;     // +0x18
    uint32_t extra;          // +0x1c 0 for compact entries
    uint32_t drive_value;    // +0x20 result of Drive virtual slot 16 (vtable +0x40)
    uint32_t owner_word_a;   // +0x24
    uint32_t owner_word_b;   // +0x28
};
static_assert(sizeof(CementFileRange) == 0x2c, "CementFileRange layout");

// Original: FUN_10651130 (rva 0x00651130), thiscall, ret 20h. Calls drive slot 0 first (reference), walks the mount
// chain calling slot 32 twice per level (as the original does), then reads slot 16. `tag` is accepted and ignored.
void __thiscall CementFileRangeInit(CementFileRange* self, Drive* drive, uint32_t offset, uint32_t size,
                                    uint32_t entry_size, uint32_t extra, uint32_t owner_word_a, uint32_t owner_word_b,
                                    uint32_t tag);

}  // namespace core
