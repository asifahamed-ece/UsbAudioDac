#ifndef __AUDIO_FFT_H__
#define __AUDIO_FFT_H__

#include <stdint.h>

#define AUDIO_FFT_BANDS     12
/* Max band height in pixels. MUST match the visualizer's bar stack (84 px
 * = exactly 12 LED blocks of 7 px), otherwise bars either clip against the
 * panel or leave dead space. Lowering this rescales the dB window: the same
 * level now reads as a slightly smaller number. */
#define AUDIO_FFT_MAX_HEIGHT 84
#define AUDIO_FFT_N         1024

void AudioFFT_Init(void);
void AudioFFT_PutSamples(const int16_t *samples, uint16_t count);
uint8_t AudioFFT_FrameReady(void);

/* Run FFT on the pending frame and emit 12 band heights (0–84).
 * Heights are absolute dBFS levels (a full-scale tone reads ~90), so
 * lowering the host volume lowers the bars. When no new frame is
 * pending (the USB stream stalled), all bands still decay toward zero —
 * call it from the visualizer at its update rate either way.
 * Must only be called when AudioFFT_FrameReady() returns 1 to run the
 * transform; otherwise bands simply ease toward silence. */
void AudioFFT_Process(uint8_t out_bands[AUDIO_FFT_BANDS]);

#endif /* __AUDIO_FFT_H__ */
