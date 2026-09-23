/* Core/Src/visualizer.c
 * 16-band differential neon spectrum visualizer (ST7735 128x128).
 */

#include "visualizer.h"
#include "st7735.h"
#include "audio_fft.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

/* Layout (spec §4.1) */
#define SEP_Y           19
#define BARS_TOP        20      /* first pixel row of bar area */
#define BASELINE_Y      119
#define BAR_W           6
#define BAR_GAP         1
#define MARGIN_X        8
#define NBANDS          16
#define MAX_BAR_H       90

/* Colors (RGB565, from spec §4.2) */
#define COL_BG          0x0000  /* black */
#define COL_HEADER_ACC  0x073E  /* bright cyan */
#define COL_SEP         0x2104  /* dark slate */
#define COL_UPPER       0xF80F  /* magenta (top 30%) */
#define COL_MID         0x07BF  /* neon cyan (mid 40%) */
#define COL_LOWER       0x280C  /* deep indigo (base 30%) */
#define COL_PEAK        0xFFFF  /* white peak dot */
#define COL_BASELINE    0x10A2  /* dark baseline */
#define COL_TEXT        0x073E  /* header text */
#define COL_TEXT_DIM    0x7BEF  /* dim gray */

/* Gradient zone boundaries in screen rows (zone-relative 27 / 63) */
#define ZONE_MID_Y      (BARS_TOP + 27)
#define ZONE_UPPER_Y    (BARS_TOP + 63)

#define Y_BOTTOM        (BARS_TOP + MAX_BAR_H)

/* State (static, in file scope — main-loop only, no concurrency) */
static uint8_t  rendered_h[NBANDS];   /* last drawn bar height */
static uint8_t  peak_h[NBANDS];       /* peak-hold height */
static uint8_t  peak_hold[NBANDS];    /* frames remaining before gravity */
static uint8_t  bands[NBANDS];        /* current FFT band heights */
static uint32_t last_update_ms;

static uint16_t bar_zone_color(int16_t y)
{
    if (y < ZONE_MID_Y) {
        return COL_UPPER;
    }
    if (y < ZONE_UPPER_Y) {
        return COL_MID;
    }
    return COL_LOWER;
}

/* Draw absolute rows [y0, y1) of one bar, split into at most 3 zone rects. */
static void draw_bar_segment(int16_t x, int16_t y0, int16_t y1)
{
    while (y0 < y1) {
        uint16_t c = bar_zone_color(y0);
        int16_t y_end;

        if (c == COL_UPPER) {
            y_end = ZONE_MID_Y;
        } else if (c == COL_MID) {
            y_end = ZONE_UPPER_Y;
        } else {
            y_end = y1;
        }
        if (y_end > y1) {
            y_end = y1;
        }
        ST7735_FillRect(x, y0, BAR_W, (int16_t)(y_end - y0), c);
        y0 = y_end;
    }
}

void Visualizer_Init(void)
{
    int i;

    ST7735_Init();

    /* Splash */
    ST7735_FillScreen(COL_BG);
    ST7735_DrawString(25, 60, "USB AUDIO DAC", COL_HEADER_ACC, COL_BG);
    HAL_Delay(1000);
    ST7735_FillScreen(COL_BG);

    /* Static chrome: header text, separator, baseline, tick labels */
    ST7735_DrawString(2, 5, "USB AUDIO", COL_TEXT, COL_BG);
    ST7735_DrawString(96, 5, "44.1k", COL_TEXT, COL_BG);
    ST7735_DrawHLine(0, SEP_Y, ST7735_WIDTH, COL_SEP);
    ST7735_DrawHLine(0, BASELINE_Y, ST7735_WIDTH, COL_BASELINE);
    ST7735_DrawString(5, 120, "60", COL_TEXT_DIM, COL_BG);
    ST7735_DrawString(47, 120, "1k", COL_TEXT_DIM, COL_BG);
    ST7735_DrawString(107, 120, "16k", COL_TEXT_DIM, COL_BG);

    /* Zero all state */
    for (i = 0; i < NBANDS; i++) {
        rendered_h[i] = 0;
        peak_h[i] = 0;
        peak_hold[i] = 0;
        bands[i] = 0;
    }
    last_update_ms = 0;
}

void Visualizer_Update(void)
{
    uint32_t now = HAL_GetTick();
    int i;

    /* Rate limit: 50 Hz cap */
    if ((now - last_update_ms) < 20U) {
        return;
    }

    if (AudioFFT_FrameReady()) {
        AudioFFT_Process(bands);
    }

    for (i = 0; i < NBANDS; i++) {
        int16_t x = (int16_t)(MARGIN_X + i * (BAR_W + BAR_GAP));
        uint8_t old_h = rendered_h[i];
        uint8_t old_peak = peak_h[i];
        uint8_t new_h = bands[i];

        if (new_h > AUDIO_FFT_MAX_HEIGHT) {
            new_h = AUDIO_FFT_MAX_HEIGHT;
        }

        /* Peak-hold ballistics */
        if (new_h > peak_h[i]) {
            peak_h[i] = new_h;
            peak_hold[i] = 8;
        } else if (peak_hold[i] > 0U) {
            peak_hold[i]--;
        } else if (peak_h[i] > new_h) {
            uint8_t drop = (uint8_t)(peak_h[i] - new_h);
            peak_h[i] = (uint8_t)(peak_h[i] - ((drop < 2U) ? drop : 2U));
        }
        if (peak_h[i] > AUDIO_FFT_MAX_HEIGHT) {
            peak_h[i] = AUDIO_FFT_MAX_HEIGHT;
        }
        if (old_h > AUDIO_FFT_MAX_HEIGHT) {
            old_h = AUDIO_FFT_MAX_HEIGHT;
        }
        if (old_peak > AUDIO_FFT_MAX_HEIGHT) {
            old_peak = AUDIO_FFT_MAX_HEIGHT;
        }

        /* Differential render of bar */
        if (new_h > old_h) {
            draw_bar_segment(x,
                             (int16_t)(Y_BOTTOM - new_h),
                             (int16_t)(Y_BOTTOM - old_h));
        } else if (new_h < old_h) {
            ST7735_FillRect(x,
                            (int16_t)(Y_BOTTOM - old_h),
                            BAR_W,
                            (int16_t)(old_h - new_h),
                            COL_BG);
        }

        /* Differential render of peak dot.
         * Restore the old dot row: gradient if the new bar covers it,
         * background otherwise (avoids holes when the bar grows past
         * a peak that sat on the old bar top). */
        if (old_peak > 0U) {
            int16_t py = (int16_t)(Y_BOTTOM - old_peak);
            uint16_t c = (old_peak <= new_h) ? bar_zone_color(py) : COL_BG;
            ST7735_FillRect(x, py, BAR_W, 1, c);
        }
        if (peak_h[i] > 0U) {
            ST7735_FillRect(x,
                            (int16_t)(Y_BOTTOM - peak_h[i]),
                            BAR_W, 1, COL_PEAK);
        }

        rendered_h[i] = new_h;
    }

    last_update_ms = now;
}
