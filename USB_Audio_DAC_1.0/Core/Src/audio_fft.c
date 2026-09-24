/* Core/Src/audio_fft.c
 *
 * 256-point real FFT → 16 log-spaced band heights for the TFT visualizer.
 *
 * CONCURRENCY (I2S DMA ISR vs main loop)
 * ---------------------------------------
 *   AudioFFT_PutSamples runs in DMA1_Stream4 ISR (priority 0).
 *   AudioFFT_FrameReady / AudioFFT_Process run in the main loop.
 *
 *   Double-buffer protocol (single writer / single reader, no locks):
 *     - ISR fills tap_buf[write_idx]; on completion it flips write_idx
 *       and sets frame_ready ONLY if the previous frame was consumed.
 *     - While frame_ready == 1, write_idx is stable (ISR will not flip);
 *       main may safely read tap_buf[1 - write_idx], then clear frame_ready.
 *     - If the ISR finishes a frame before main clears frame_ready, that
 *       frame is dropped (fill restarts on the unpublished slot) — no
 *       shared multi-byte state is ever handed off mid-update.
 *
 * MEMORY
 * ------
 *   Working buffers live in .ccmram (CPU-only, outside the 128 KB SRAM
 *   budget). CCMRAM is NOLOAD — AudioFFT_Init must zero every buffer;
 *   startup does not clear it. None of these buffers are DMA-reachable.
 *
 *   PutSamples is an integer O(n) tap only: no float, no FFT.
 *
 * MAPPING
 * -------
 *   Per-frame peak normalization: magnitudes are divided by the frame's
 *   peak bin before the dB map, so Hann leakage stays a fixed number of
 *   dB below the tone at any absolute level (full-scale vs quiet). A
 *   linear dB window of AUDIO_FFT_DB_RANGE maps [−RANGE, 0] dB → [0, 90].
 */

#include "audio_fft.h"
#include "arm_math.h"
#include <math.h>
#include <string.h>

#define AUDIO_FFT_DB_RANGE  60.0f
#define AUDIO_FFT_DECAY     0.78f
#define AUDIO_FFT_PI        3.14159265358979323846f

/* Absolute dBFS reference for the band mapping. A full-scale sine
 * (amplitude 32767) produces a Hann-windowed FFT magnitude of
 * A * N/2 * 0.5 at its bin, so its average band reads 0 dB and maps to
 * full height. Real playback levels land below this, which makes the
 * display respond to the laptop's volume instead of renormalizing every
 * frame against the loudest bin (which kept bars near-full at any
 * non-zero volume). */
#define AUDIO_FFT_FULL_SCALE  (32767.0f * 0.5f * (float)(AUDIO_FFT_N / 2U))

/* ISR double-buffer + main-loop FFT scratch.
 * Note: STM32F411 has NO CCMRAM (only F405/407/429 do). Buffers live in
 * normal SRAM .bss. The memsets in AudioFFT_Init still zero them. */
static int16_t tap_buf[2][AUDIO_FFT_N];
static float   hann_window[AUDIO_FFT_N];
static float   fft_in[AUDIO_FFT_N];
static float   fft_out[AUDIO_FFT_N];
static float   mag[AUDIO_FFT_N / 2U];
static float   band_current[AUDIO_FFT_BANDS];

static arm_rfft_fast_instance_f32 rfft_instance;

/* Handoff flags: written by ISR and/or main, never both for the same
 * role — volatile so the main loop cannot cache a stale frame_ready. */
static volatile uint8_t write_idx;
static volatile uint8_t frame_ready;
static uint16_t fill_count;   /* ISR-owned only */

/* Design-spec band edges (60 Hz → 17.5 kHz) as bin indices at
 * Fs = 44100 Hz, N = 256 (df ≈ 172.27 Hz). Band 0 starts at bin 1
 * (skips DC); band 5 holds bin 6 ≈ 1034 Hz (the 1 kHz tone). */
static const uint16_t band_start[AUDIO_FFT_BANDS + 1U] = {
    1U, 2U, 3U, 4U, 5U, 6U, 7U, 10U,
    13U, 18U, 24U, 31U, 40U, 52U, 66U, 82U, 102U
};

void AudioFFT_Init(void)
{
    uint32_t n;

    write_idx    = 0U;
    frame_ready  = 0U;
    fill_count   = 0U;

    /* Zero all FFT buffers explicitly (belt-and-suspenders; .bss is cleared
     * by startup, but explicit zeroing documents the initialization contract). */
    memset(tap_buf, 0, sizeof(tap_buf));
    memset(band_current, 0, sizeof(band_current));
    memset(fft_in, 0, sizeof(fft_in));
    memset(fft_out, 0, sizeof(fft_out));
    memset(mag, 0, sizeof(mag));

    /* Defensive re-clear: if a PutSamples ISR ever raced Init (it
     * must not — main.c arms FFT before AudioI2S_Init), fill_count
     * could desync from the memset above. Belt-and-suspenders. */
    fill_count = 0U;
    frame_ready = 0U;

    /* Symmetric Hann window (runtime-generated: avoids a 1 KB flash table). */
    for (n = 0U; n < (uint32_t)AUDIO_FFT_N; n++) {
        hann_window[n] = 0.5f * (1.0f - cosf(2.0f * AUDIO_FFT_PI *
                                              (float)n /
                                              (float)(AUDIO_FFT_N - 1U)));
    }

    /* N=256 is a supported RFFT size; init cannot fail for this build. */
    (void)arm_rfft_fast_init_f32(&rfft_instance, (uint16_t)AUDIO_FFT_N);
}

void AudioFFT_PutSamples(const int16_t *samples, uint16_t count)
{
    uint16_t i;

    for (i = 0U; i < count; i++) {
        tap_buf[write_idx][fill_count] = samples[i];
        fill_count++;

        if (fill_count == (uint16_t)AUDIO_FFT_N) {
            fill_count = 0U;
            /* Publish only when the consumer has cleared the previous
             * frame; otherwise drop and refill the unpublished slot. */
            if (frame_ready == 0U) {
                write_idx ^= 1U;
                frame_ready = 1U;
            }
        }
    }
}

uint8_t AudioFFT_FrameReady(void)
{
    return frame_ready;
}

void AudioFFT_Process(uint8_t out_bands[AUDIO_FFT_BANDS])
{
    if (frame_ready != 0U) {
        /* write_idx is stable while frame_ready == 1 (ISR will not flip). */
        const int16_t *frame = tap_buf[1U - write_idx];
        uint32_t i;
        uint8_t  b;

        for (i = 0U; i < (uint32_t)AUDIO_FFT_N; i++) {
            fft_in[i] = (float)frame[i] * hann_window[i];
        }

        arm_rfft_fast_f32(&rfft_instance, fft_in, fft_out, 0U);
        arm_cmplx_mag_f32(fft_out, mag, (uint32_t)(AUDIO_FFT_N / 2U));

        for (b = 0U; b < (uint8_t)AUDIO_FFT_BANDS; b++) {
            uint16_t lo  = band_start[b];
            uint16_t hi  = band_start[b + 1U];
            uint16_t k;
            float    sum    = 0.0f;
            float    avg;
            float    target = 0.0f;

            for (k = lo; k < hi; k++) {
                sum += mag[k];
            }
            avg = sum / (float)(hi - lo);

            {
                /* Level relative to full scale (0 dBFS = full bar). */
                float ratio = avg / AUDIO_FFT_FULL_SCALE;
                if (ratio >= 1.0e-10f) {
                    /* Map [−RANGE, 0] dBFS → [0, MAX_HEIGHT]. */
                    float db = 20.0f * log10f(ratio);
                    target = (db + AUDIO_FFT_DB_RANGE) *
                             ((float)AUDIO_FFT_MAX_HEIGHT /
                              AUDIO_FFT_DB_RANGE);
                }
            }

            if (target < 0.0f) {
                target = 0.0f;
            }
            if (target > (float)AUDIO_FFT_MAX_HEIGHT) {
                target = (float)AUDIO_FFT_MAX_HEIGHT;
            }

            /* Instant attack, exponential decay. */
            if (target > band_current[b]) {
                band_current[b] = target;
            } else {
                band_current[b] *= AUDIO_FFT_DECAY;
            }
        }

        frame_ready = 0U;
    } else {
        /* Idle: the USB stream stalled (host paused playback and stopped
         * delivering audio). Decay every band toward zero so the display
         * falls to a clean black background instead of freezing at the
         * last levels. */
        uint8_t b;
        for (b = 0U; b < (uint8_t)AUDIO_FFT_BANDS; b++) {
            band_current[b] *= AUDIO_FFT_DECAY;
        }
    }

    {
        uint8_t b;
        for (b = 0U; b < (uint8_t)AUDIO_FFT_BANDS; b++) {
            float v = band_current[b];
            if (v < 0.0f) {
                v = 0.0f;
            }
            if (v > (float)AUDIO_FFT_MAX_HEIGHT) {
                v = (float)AUDIO_FFT_MAX_HEIGHT;
            }
            out_bands[b] = (uint8_t)v;
        }
    }
}
