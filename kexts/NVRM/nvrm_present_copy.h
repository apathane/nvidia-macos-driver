#pragma once
#include <stdint.h>
#include <stddef.h>

// Mapped framebuffer memory, not device registers. Caller owns synchronization
// with the producer and validates the complete source/destination extents.
// No SIMD/FPU state, allocation, speculative overread or writes past rowBytes.
static inline void nvrmPresentCopyRow(volatile void *destination,
                                     const volatile void *source,
                                     size_t rowBytes, int mode)
{
    volatile uint32_t *d = (volatile uint32_t *)destination;
    const volatile uint32_t *s = (const volatile uint32_t *)source;
    // Rows and pointers must be word-aligned. Use the old access width if either
    // endpoint is not qword-aligned; this includes alternating padded rows.
    if (mode && !(((uintptr_t)d | (uintptr_t)s) & 7)) {
        size_t count = rowBytes / 8;
        volatile uint64_t *dq = (volatile uint64_t *)d;
        const volatile uint64_t *sq = (const volatile uint64_t *)s;
        if (mode == 2) {
#if defined(__x86_64__)
            // REP MOVSQ uses integer state only. Explicit memory clobber keeps
            // producer reads and framebuffer writes inside the timed interval.
            __asm__ volatile("cld; rep movsq" : "+D"(dq), "+S"(sq), "+c"(count) : : "memory", "cc");
#else
            while (count--) *dq++ = *sq++;
#endif
        } else {
            while (count >= 8) {
                for (unsigned i = 0; i < 8; ++i) dq[i] = sq[i];
                dq += 8; sq += 8; count -= 8;
            }
            while (count--) *dq++ = *sq++;
        }
        if (rowBytes & 4) *(volatile uint32_t *)dq = *(const volatile uint32_t *)sq;
    } else {
        for (size_t i = 0; i < rowBytes / 4; ++i) d[i] = s[i];
    }
}
static inline void nvrmPresentCopyFinish()
{
#if defined(__x86_64__)
    // Drain WC stores before reporting completion; this is not a display-vblank
    // wait and cannot by itself make writes to the active scanout tear-free.
    __asm__ volatile("mfence" ::: "memory");
#endif
}
