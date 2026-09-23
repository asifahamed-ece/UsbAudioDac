/* tests/test_audio_fft.c
 *
 * Host-simulated unit tests for the 256-point FFT band-binning module.
 *
 * Links the pure-C CMSIS-DSP paths (TransformFunctions etc. compile on
 * host gcc; the RFFT/cmplx_mag code has no ARM asm on the f32 scalar path).
 * Build with `make -C tests run` from USB_Audio_DAC_1.0/.
 *
 * Band map (design spec, Fs=44.1 kHz, N=256, df≈172.27 Hz):
 *   band 0: 60–180 Hz   -> bins [1,2)
 *   band 5: 900–1200 Hz -> bins [6,7)   (1 kHz lands here)
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

/* Full-scale Hann leakage puts ~79 into band 0 at K=22, which would
 * fail the band0<20 assertion. A modest amplitude keeps far-out bins
 * under the threshold while band 5 still exceeds 40. */
#define SINE_AMP   50
#define SINE_HZ    1000.0f
#define SAMPLE_RATE 44100.0f
#define PI_F       3.14159265358979323846f

static void fill_sine(int16_t *buf, int16_t amp, float hz)
{
    int n;
    for (n = 0; n < AUDIO_FFT_N; n++) {
        float t = 2.0f * PI_F * hz * (float)n / SAMPLE_RATE;
        buf[n] = (int16_t)((float)amp * sinf(t));
    }
}

static void test_1khz_sine_maps_to_band5(void)
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

    /* 1 kHz falls in band 5 (900 Hz–1.2 kHz). */
    CHECK(bands[5] > 40);
    /* Band 0 (60–180 Hz) must stay quiet. */
    CHECK(bands[0] < 20);

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
    /* Process with no frame must not invent energy. */
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
    RUN(1khz_sine_maps_to_band5);
    RUN(all_zero_input_yields_zero_bands);
    RUN(partial_fill_not_ready);

    printf("\n=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
