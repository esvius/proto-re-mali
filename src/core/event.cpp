#include "event.h"

#include <windows.h>

#include "../host/hooks.h"

namespace core {

namespace {

constexpr uint32_t kTlsIndexRva = 0x0127a470;

volatile LONG* Counter(CountedEvent* e) {
    return reinterpret_cast<volatile LONG*>(&e->count);
}

// Atomic read through a compare-exchange loop, as the original does (value & -1).
uint32_t Load(CountedEvent* e) {
    LONG value;
    do {
        value = *Counter(e);
    } while (InterlockedCompareExchange(Counter(e), value, value) != value);
    return static_cast<uint32_t>(value);
}

}  // namespace

void CountedEventCreate(CountedEvent* e, uint32_t initial) {
    e->count = initial;
    e->handle = CreateEventA(nullptr, TRUE, initial == 0, nullptr);
}

void CountedEventDestroy(CountedEvent* e) {
    WaitForSingleObject(e->handle, INFINITE);
    CloseHandle(e->handle);
}

void CountedEventReset(CountedEvent* e, uint32_t count) {
    InterlockedCompareExchange(Counter(e), static_cast<LONG>(count), 0);
    if (count != 0) {
        WaitForSingleObject(e->handle, INFINITE);
        ResetEvent(e->handle);
    }
}

uint32_t CountedEventSubtract(CountedEvent* e, uint32_t amount) {
    uint32_t previous = Load(e);
    for (;;) {
        uint32_t next = previous > amount ? previous - amount : 0;
        LONG seen = InterlockedCompareExchange(Counter(e), static_cast<LONG>(next), static_cast<LONG>(previous));
        if (static_cast<uint32_t>(seen) == previous) {
            return previous;
        }
        previous = static_cast<uint32_t>(seen);
    }
}

void CountedEventSignal(CountedEvent* e, uint32_t amount) {
    if (amount == 0) {
        return;
    }
    uint32_t previous = CountedEventSubtract(e, amount);
    if (previous != 0 && amount >= previous) {
        SetEvent(e->handle);
    }
}

void CountedEventWait(CountedEvent* e) {
    if (Load(e) != 0) {
        WaitForSingleObject(e->handle, INFINITE);
    }
}

void* ThreadLocalValue() {
    return TlsGetValue(*static_cast<DWORD*>(protore::EngineAddress(kTlsIndexRva)));
}

}  // namespace core
