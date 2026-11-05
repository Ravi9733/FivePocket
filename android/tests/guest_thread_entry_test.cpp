#include "guest_thread_entry.h"

#include <cassert>

using namespace recomp::bootstrap;

int main() {
    GuestPreparedThread thread{};
    thread.layout.stackBase = 0x64000000;
    thread.layout.stackSize = 0x10000;
    thread.layout.pcrAddress = 0x64011000;
    thread.startRoutine = 0x82001000;
    thread.startContext = 0xABCD;
    auto entry = makeGuestThreadEntry(thread);
    assert(entry && entry->directEntry && entry->address == thread.startRoutine);
    assert(entry->stackPointer == 0x6400FF50 && (entry->stackPointer & 15) == 0);
    assert(entry->pcrAddress == thread.layout.pcrAddress);
    assert(entry->argument0 == 0xABCD && entry->argument1 == 0);
    thread.xapiStartup = 0x82002000;
    entry = makeGuestThreadEntry(thread);
    assert(entry && !entry->directEntry && entry->address == thread.xapiStartup);
    assert(entry->argument0 == thread.startRoutine && entry->argument1 == thread.startContext);
    const auto valid = thread;
    thread.xapiStartup |= 1;
    assert(!makeGuestThreadEntry(thread));
    thread = valid; thread.layout.pcrAddress = 0;
    assert(!makeGuestThreadEntry(thread));
    thread = valid; thread.layout.pcrAddress |= 1;
    assert(!makeGuestThreadEntry(thread));
    thread = valid; thread.layout.stackSize = 160;
    assert(!makeGuestThreadEntry(thread));
    thread = valid; thread.layout.stackSize = 176;
    assert(makeGuestThreadEntry(thread)->stackPointer == thread.layout.stackBase);
    thread = valid; thread.layout.stackSize -= 1;
    assert(!makeGuestThreadEntry(thread));
    thread = valid; thread.layout.stackBase = 0xFFFF0000;
    assert(!makeGuestThreadEntry(thread));
    thread = valid; thread.xapiStartup = thread.startRoutine = 0;
    assert(!makeGuestThreadEntry(thread));
}
