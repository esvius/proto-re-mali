#include "cement_file_range.h"

namespace core {

namespace {

constexpr int kSlotReference = 0;   // vtable +0x00
constexpr int kSlotDriveValue = 16; // vtable +0x40
constexpr int kSlotMount = 32;      // vtable +0x80

using ReferenceFn = void(__thiscall*)(Drive*);
using DriveValueFn = uint32_t(__thiscall*)(Drive*);
using MountFn = DriveMount*(__thiscall*)(Drive*);

template <typename Fn>
Fn Slot(Drive* drive, int index) {
    void** vtable = *reinterpret_cast<void***>(drive);
    return reinterpret_cast<Fn>(vtable[index]);
}

}  // namespace

void __thiscall CementFileRangeInit(CementFileRange* self, Drive* drive, uint32_t offset, uint32_t size,
                                    uint32_t entry_size, uint32_t extra, uint32_t owner_word_a, uint32_t owner_word_b,
                                    uint32_t /*tag*/) {
    Slot<ReferenceFn>(drive, kSlotReference)(drive);
    self->drive = drive;
    self->offset = offset;
    Drive* level = drive;
    do {
        self->offset += Slot<MountFn>(level, kSlotMount)(level)->base_offset;
        level = Slot<MountFn>(level, kSlotMount)(level)->parent;
    } while (level);
    self->offset_high = 0;
    self->position = 0;
    self->size_high = 0;
    self->size = size;
    self->extra = extra;
    self->entry_size = entry_size;
    self->drive_value = Slot<DriveValueFn>(drive, kSlotDriveValue)(drive);
    self->owner_word_a = owner_word_a;
    self->owner_word_b = owner_word_b;
}

}  // namespace core
