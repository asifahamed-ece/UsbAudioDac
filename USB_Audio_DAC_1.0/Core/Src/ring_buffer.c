/* Core/Src/ring_buffer.c
 *
 * SPSC ring buffer for USB → I2S audio. See ring_buffer.h for the
 * invariant and design notes.
 *
 * CONCURRENCY
 * -----------
 *   head is only written by the producer (USB ISR).
 *   tail is only written by the consumer (I2S DMA half/full ISR).
 *   They never touch the same memory location, so no spinlock
 *   or __disable_irq is needed.
 *
 *   The producer reads tail (to check "is the ring full?") and
 *   the consumer reads head (to check "is the ring empty?").
 *   On Cortex-M4, a 16-bit aligned load is atomic — you cannot
 *   see a half-written value. So reading the other side's index
 *   is also safe.
 *
 *   Caveat: if any of these functions are ever called from main
 *   loop while the ISRs are running, wrap the index update in
 *   __disable_irq / __enable_irq. Currently we only call them
 *   from the two ISRs, so we're safe.
 */

#include "ring_buffer.h"

/* The single global instance. Placed in .bss by the linker
 * (zero-initialized at startup). */
RingBuffer_t audio_ring = {0};

/* --- Producer-side API ----------------------------------------- */

void RingBuffer_Reset(void)
{
    audio_ring.head = 0;
    audio_ring.tail = 0;
}

uint16_t RingBuffer_Space(void)
{
    /* RING_BUFFER_SIZE - 1 is the maximum number of in-flight
     * samples (we waste one slot to disambiguate empty vs full). */
    return (uint16_t)((RING_BUFFER_SIZE - 1) - RingBuffer_Available());
}

uint16_t RingBuffer_Write(const int16_t *src, uint16_t n_samples)
{
    uint16_t space   = RingBuffer_Space();
    uint16_t to_write = (n_samples < space) ? n_samples : space;
    uint16_t i;

    for (i = 0; i < to_write; i++) {
        audio_ring.buffer[audio_ring.head] = src[i];
        /* Wrap with & instead of %: SIZE is a power of 2, so
         * (head + 1) & (SIZE - 1) is a single-cycle AND. */
        audio_ring.head = (uint16_t)((audio_ring.head + 1) & (RING_BUFFER_SIZE - 1));
    }
    return to_write;
}

/* --- Consumer-side API ----------------------------------------- */

uint16_t RingBuffer_Available(void)
{
    /* head and tail are both uint16_t. The producer never lets
     * head run more than SIZE-1 ahead of tail, so the subtraction
     * (head - tail) gives the modular difference correctly even
     * across the wraparound. We mask to keep the result in
     * [0, SIZE-1] for cleanliness. */
    return (uint16_t)((audio_ring.head - audio_ring.tail) & (RING_BUFFER_SIZE - 1));
}

uint16_t RingBuffer_Read(int16_t *dst, uint16_t n_samples)
{
    uint16_t avail   = RingBuffer_Available();
    uint16_t to_read = (n_samples < avail) ? n_samples : avail;
    uint16_t i;

    for (i = 0; i < to_read; i++) {
        dst[i] = audio_ring.buffer[audio_ring.tail];
        audio_ring.tail = (uint16_t)((audio_ring.tail + 1) & (RING_BUFFER_SIZE - 1));
    }
    return to_read;
}
