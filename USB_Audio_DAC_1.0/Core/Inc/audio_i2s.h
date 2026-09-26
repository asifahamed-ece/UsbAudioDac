#ifndef __AUDIO_I2S_H__
#define __AUDIO_I2S_H__

#include <stdint.h>

/* I2S DMA buffer sizing.
 *
 * The I2S2 peripheral streams audio via DMA1_Stream4 in 16-bit
 * elements. The HAL splits our buffer into two halves and pings
 * between them: it fires the Half-transfer callback after the
 * first half is consumed, and the Full-transfer callback after
 * the second half. So one full buffer = two callbacks = 2× the
 * half-period.
 *
 * AUDIO_I2S_BUFFER_SIZE = 880 int16 = 440 stereo frames = 9.2 ms @ 48 kHz.
 * Half = 440 int16 = 220 stereo frames = 5 ms ≈ 5 USB packets of mono.
 *
 * MUST be a multiple of 4 so the DMA half (size/2) is an even number of
 * halfwords = a whole number of stereo frames.  882 produced a 441-word
 * half that split a stereo frame at every 5 ms boundary -> a 200 Hz/100 Hz
 * glitch (confirmed by recording analysis).  880 -> half = 440 words =
 * exactly 220 frames, aligned with the 220-mono refill.
 *
 * We pick 10 ms so that the ring (which holds 46 ms) has plenty
 * of headroom even if a refill is delayed by an interrupt.
 */
#define AUDIO_I2S_BUFFER_SIZE 880

/* MEMORY-PLACEMENT RULE
 * -------------------
 * This buffer MUST stay in main SRAM (0x20000000..0x20020000).
 * DMA1_Stream4 reads from it; DMA only sees AHB addresses, not
 * CCMRAM. The same warning that lives in main.c applies here.
 */
extern int16_t audio_i2s_buffer[AUDIO_I2S_BUFFER_SIZE];

/* One-time init: zero the I2S buffer, start the DMA in circular
 * mode. Called from main.c BEFORE MX_USB_DEVICE_Init() so the
 * I2S stream is already running (and outputting silence) by the
 * time the USB host opens its audio pipe. */
void AudioI2S_Init(void);

/* Refill the first half of the I2S DMA buffer (samples 0..440)
 * from the ring. Called by the HAL I2S Half-transfer callback
 * (routed through stm32f4xx_it.c).
 *
 * Pulls up to 220 mono samples from the ring and duplicates each
 * into L+R slots. If the ring is empty (underrun), the half
 * stays zeroed — silence instead of held DC. */
void AudioI2S_RefillHalfA(void);

/* Refill the second half (samples 440..879). Same contract as
 * HalfA but writes to the upper half of the buffer. */
void AudioI2S_RefillHalfB(void);

#endif /* __AUDIO_I2S_H__ */
