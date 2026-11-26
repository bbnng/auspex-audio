#pragma once

namespace sa {

// Steam Audio 4.8's parametric reverb (and with it the hybrid one) starts with whatever its memory held before, and
// iplReflectionEffectReset does not clear that. Reused heap memory then comes out as an offset of up to 1e35: full scale
// noise after one in twenty rebuilds, and possible at the first creation, too. iplContextCreate's allocator does not help:
// that part is allocated with plain `new`. So while anyone holds this, everything phonon.dll allocates is zeroed: its own
// imports of HeapAlloc and HeapReAlloc add HEAP_ZERO_MEMORY. No other module is affected.
//
// Acquire before the first Steam Audio call, release after the last one. False: phonon.dll is not as expected, and nothing
// is changed.
bool acquire_zeroed_phonon_memory();
void release_zeroed_phonon_memory();

} // namespace sa
