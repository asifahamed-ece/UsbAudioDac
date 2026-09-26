#ifndef __RING_BUFFER_H__
#define __RING_BUFFER_H__

#include <stdint.h>
#include <stddef.h>

/* Single-producer / single-consumer ring buffer for USB → I2S audio.
 *
 * INVARIANT
 * ---------
 *   head == tail                -> ring is empty
 *   (head + 1) & (SIZE - 1) == tail -> ring is full
 *
 * The full-check wastes exactly 1 sample slot; this is the
 * standard SPSC ring-buffer trick to disambiguate empty vs full
 * without a separate count variable.
 *
 * SIZE MUST be a power of 2 so that `% SIZE` and `& (SIZE-1)`
 * are interchangeable. We always use the `&` form in the .c
 * because the compiler can't always prove the modulo is a power
 * of 2 from the macro alone.
 *
 * SIZING
 * ------
 *   2048 samples * (1 / 48000 Hz) = 42.7 ms of audio.
 *   USB Full-Speed sends 1 packet (48 samples) every 1 ms, so
 *   the ring holds ~42 packets of headroom. If the I2S consumer
 *   ever stalls for 42 ms without new USB packets, we underrun.
 *
 *   Sized >= the largest single sync push: USBD_AUDIO_Sync clamps its
 *   per-sync copy to AUDIO_TOTAL_BUF_SIZE/2 (3840 mono samples), and
 *   AUDIO_AudioCmd_FS(PLAY) caps at RING_BUFFER_SIZE-1.  Keeping the
 *   ring >= that worst case + backpressure in Sync (only consume what
 *   the ring can accept) makes the path lossless instead of silently
 *   dropping overflow. Must stay a power of two.
 */
#define RING_BUFFER_SIZE 2048

typedef struct {
    int16_t           buffer[RING_BUFFER_SIZE];
    volatile uint16_t head;   /* Producer writes here.  */
    volatile uint16_t tail;   /* Consumer writes here.  */
} RingBuffer_t;

/* The one and only ring instance, defined in ring_buffer.c. */
extern RingBuffer_t audio_ring;

/* Producer-side API (runs in the I2S DMA ISR via USBD_AUDIO_Sync, and in
 * the OTG_FS control path for Reset alone -- see the hazard note in .c) */
void     RingBuffer_Reset(void);
uint16_t RingBuffer_Write(const int16_t *src, uint16_t n_samples);

/* Consumer-side API (I2S DMA ISR) */
uint16_t RingBuffer_Read(int16_t *dst, uint16_t n_samples);
uint16_t RingBuffer_Available(void);
uint16_t RingBuffer_Space(void);

#endif /* __RING_BUFFER_H__ */
