#ifndef __AUDIO_FFT_H__
#define __AUDIO_FFT_H__

#include <stdint.h>

#define AUDIO_FFT_BANDS     16
#define AUDIO_FFT_MAX_HEIGHT 90
#define AUDIO_FFT_N         256

void AudioFFT_Init(void);
void AudioFFT_PutSamples(const int16_t *samples, uint16_t count);
uint8_t AudioFFT_FrameReady(void);
void AudioFFT_Process(uint8_t out_bands[AUDIO_FFT_BANDS]);

#endif /* __AUDIO_FFT_H__ */
