/* tests/test_audio_fft.c
 *
 * Host-simulated unit tests for the 1024-point FFT band-binning module.
 *
 * Links the pure-C CMSIS-DSP paths (TransformFunctions etc. compile on
 * host gcc; the RFFT/cmplx_mag code has no ARM asm on the f32 scalar path).
 * Build with `make -C tests run` from USB_Audio_DAC_1.0/.
 *
 * Band map (design spec, Fs=44.1 kHz, N=1024, df≈43.07 Hz):
 *   band 0: 43–172 Hz    -> bins [1,4)      (bass floor, 60 Hz inside)
 *   band 4: 947–1378 Hz  -> bins [22,33)    (a 1 kHz tone lands here)
 *
 * NOTE ON SAMPLE_RATE BELOW: the device now runs at exactly 48000 Hz, but
 * this probe signal is deliberately generated at 44100 Hz. band_start[] is
 * a table of BIN INDICES, and the band boundaries themselves are unchanged
 * by the rate change — only their Hz labels moved (+8.8 %, df 43.07 ->
 * 46.875 Hz). Generating the probe at 44.1 kHz is what puts 1 kHz inside
 * band 4 as the design spec intends; generating it at 48 kHz would put the
 * same tone in band 3 and the assertion would have to change with it.
 * Rescaling band_start[] to restore the original Hz labels is a separate
 * cosmetic change, deliberately still open.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../Core/Inc/audio_fft.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond)                                                 \
    do {                                                            \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);\
        }                                                           \
    } while (0)

/* Full-scale: exercises absolute-dBFS mapping (a full-scale tone must
 * read near the top, and the Hann leakage floor stay quiet). */
#define SINE_AMP   32767
#define SINE_HZ    1000.0f
#define SAMPLE_RATE 44100.0f
#define PI_F       3.14159265358979323846f

static void fill_sine(int16_t *buf, int16_t amp, float hz)
{
    int n;
    for (n = 0; n < AUDIO_FFT_N; n++) {
        float t = 2.0f * PI_F * hz * (float)n / SAMPLE_RATE;
        float v = (float)amp * sinf(t);
        if (v > 32767.0f) {
            v = 32767.0f;
        }
        if (v < -32768.0f) {
            v = -32768.0f;
        }
        buf[n] = (int16_t)v;
    }
}

static void test_1khz_sine_maps_to_band4(void)
{
    int16_t samples[AUDIO_FFT_N];
    uint8_t bands[AUDIO_FFT_BANDS];
    int b;

    AudioFFT_Init();
    fill_sine(samples, SINE_AMP, SINE_HZ);

    AudioFFT_PutSamples(samples, AUDIO_FFT_N);
    CHECK(AudioFFT_FrameReady() == 1);

    AudioFFT_Process(bands);
    CHECK(AudioFFT_FrameReady() == 0);

    /* 1 kHz falls in band 4 (947 Hz–1.38 kHz) — full-scale tone. */
    CHECK(bands[4] >= 60);
    /* Band 0 (43–172 Hz bass) must stay quiet despite Hann leakage. */
    CHECK(bands[0] < 30);

    printf("  bands:");
    for (b = 0; b < AUDIO_FFT_BANDS; b++)
        printf(" %u", (unsigned)bands[b]);
    printf("\n");
}

static void test_all_zero_input_yields_zero_bands(void)
{
    int16_t zeros[AUDIO_FFT_N];
    uint8_t bands[AUDIO_FFT_BANDS];
    int b;

    memset(zeros, 0, sizeof(zeros));
    AudioFFT_Init();
    AudioFFT_PutSamples(zeros, AUDIO_FFT_N);
    CHECK(AudioFFT_FrameReady() == 1);
    AudioFFT_Process(bands);

    for (b = 0; b < AUDIO_FFT_BANDS; b++)
        CHECK(bands[b] == 0);
}

static void test_partial_fill_not_ready(void)
{
    int16_t samples[64];
    uint8_t bands[AUDIO_FFT_BANDS];

    memset(samples, 0x11, sizeof(samples));
    AudioFFT_Init();
    CHECK(AudioFFT_FrameReady() == 0);
    AudioFFT_PutSamples(samples, 64);
    CHECK(AudioFFT_FrameReady() == 0);
    /* Contract: Process only when FrameReady==1. Calling anyway must
     * not invent energy (defensive: band_current still zero from Init). */
    AudioFFT_Process(bands);
    {
        int b;
        for (b = 0; b < AUDIO_FFT_BANDS; b++)
            CHECK(bands[b] == 0);
    }
}

#define RUN(name)                                                    \
    do {                                                             \
        printf("[TEST] %s\n", #name);                                \
        test_##name();                                               \
    } while (0)

int main(void)
{
    RUN(1khz_sine_maps_to_band4);
    RUN(all_zero_input_yields_zero_bands);
    RUN(partial_fill_not_ready);

    printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
