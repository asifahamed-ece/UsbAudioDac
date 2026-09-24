#ifndef __AUDIO_FFT_H__
#define __AUDIO_FFT_H__

#include <stdint.h>

#define AUDIO_FFT_BANDS     12
#define AUDIO_FFT_MAX_HEIGHT 90
#define AUDIO_FFT_N         1024

void AudioFFT_Init(void);
void AudioFFT_PutSamples(const int16_t *samples, uint16_t count);
uint8_t AudioFFT_FrameReady(void);

/* Run FFT on the pending frame and emit 12 band heights (0–90).
 * Heights are absolute dBFS levels (a full-scale tone reads ~90), so
 * lowering the host volume lowers the bars. When no new frame is
 * pending (the USB stream stalled), all bands still decay toward zero —
 * call it from the visualizer at its update rate either way.
 * Must only be called when AudioFFT_FrameReady() returns 1 to run the
 * transform; otherwise bands simply ease toward silence. */
void AudioFFT_Process(uint8_t out_bands[AUDIO_FFT_BANDS]);

#endif /* __AUDIO_FFT_H__ */
