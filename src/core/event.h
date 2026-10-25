// core counted event: a manual-reset Win32 event paired with a counter of outstanding work. Signal() subtracts from
// the counter and sets the event when it reaches zero; Wait() blocks only while the counter is nonzero. Embedded in
// engine objects (e.g. core::File +0x1c). Gets a POSIX implementation on Android.
#pragma once

#include <cstdint>

namespace core {

struct CountedEvent {
    volatile uint32_t count;  // outstanding work; the event is signalled when this is zero
    void* handle;             // Win32 manual-reset event
};
static_assert(sizeof(CountedEvent) == 8, "CountedEvent layout");

// Original: FUN_1063d320 (rva 0x0063d320), cdecl. count = initial; event created signalled iff initial == 0.
void CountedEventCreate(CountedEvent* e, uint32_t initial);

// Original: FUN_1063d350 (rva 0x0063d350), cdecl. Waits for the event, then closes it.
void CountedEventDestroy(CountedEvent* e);

// Original: FUN_1063d390 (rva 0x0063d390), cdecl. If the count is 0, sets it to `count` (atomically); when `count`
// is nonzero, waits for the event and resets it.
void CountedEventReset(CountedEvent* e, uint32_t count);

// Original: FUN_1063d3d0 (rva 0x0063d3d0), cdecl. Subtracts `amount` (saturating at 0); sets the event when the
// previous count was nonzero and `amount` covered it.
void CountedEventSignal(CountedEvent* e, uint32_t amount);

// Original: FUN_1063d400 (rva 0x0063d400), cdecl. Blocks while the count is nonzero.
void CountedEventWait(CountedEvent* e);

// Original: FUN_1063d220 (rva 0x0063d220), custom convention (esi = event, edx = amount): atomic saturating
// subtraction, returns the previous count. Exposed for the reconstruction only; not hooked.
uint32_t CountedEventSubtract(CountedEvent* e, uint32_t amount);

// Original: FUN_10644dc0 (rva 0x00644dc0), cdecl. TlsGetValue of the engine's thread-local slot (index at
// rva 0x0127a470).
void* ThreadLocalValue();

}  // namespace core
