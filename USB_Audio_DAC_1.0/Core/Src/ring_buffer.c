/* Core/Src/ring_buffer.c
 *
 * SPSC ring buffer for USB → I2S audio. See ring_buffer.h for the
 * invariant and design notes.
 *
 * CONCURRENCY
 * -----------
 *   The textbook SPSC rule still holds and is the invariant to preserve:
 *   head is written only by the producer, tail only by the consumer, and
 *   they never share a location, so no lock or __disable_irq is needed
 *   for the steady-state path.
 *
 *   In THIS build the two ends are not even concurrent. Both run inside
 *   the same handler, DMA1_Stream4_IRQHandler, in a fixed order:
 *
 *     DMA1_Stream4_IRQHandler        (NVIC priority 0, main.c)
 *       1. HalfTransfer_CallBack_FS() -> USBD_AUDIO_Sync()
 *            -> AudioCmd(AUDIO_CMD_PLAY) -> RingBuffer_Write()  [producer]
 *       2. AudioI2S_RefillHalfA()   -> RingBuffer_Read()         [consumer]
 *
 *   USB audio therefore reaches the ring only by being deferred out of
 *   haudio->buffer and pushed in from the I2S DMA ISR -- NOT from
 *   OTG_FS_IRQHandler. Step 1 must stay ahead of step 2, or the consumer
 *   would drain a ring the producer has not refilled yet.
 *
 *   KNOWN HAZARD (not fixed, documented): RingBuffer_Reset() is the one
 *   call that does NOT come from that handler. It is reached via
 *   AUDIO_CMD_START on the control path, i.e. in OTG_FS_IRQHandler
 *   context at NVIC priority 3 (usbd_conf.c) -- which the priority-0 DMA
 *   handler can preempt. If preemption lands between the two stores, tail
 *   is reset after the ISR already advanced head, and the ring briefly
 *   replays stale buffer content (an audible click at stream start, not
 *   memory corruption). Making Reset atomic would mean masking interrupts
 *   around the two stores, or moving the reset onto the DMA handler.
 *
 *   Reading the other side's index is safe regardless: head and tail are
 *   uint16_t, and an aligned 16-bit load is atomic on Cortex-M4, so no
 *   half-written value is observable. RingBuffer_Space() reads both.
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
