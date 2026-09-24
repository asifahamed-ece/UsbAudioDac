/* Core/Src/visualizer.c
 * 16-band differential spectrum visualizer with Synthwave theme (ST7735 128x128).
 * Optimized for STM32F4 Cortex-M4 with smooth ballistics and loading animation.
 */

#include "visualizer.h"
#include "st7735.h"
#include "audio_fft.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <math.h>

/* Layout */
#define SEP_Y           19
#define BARS_TOP        20      /* first pixel row of bar area */
#define BASELINE_Y      119
#define BAR_W           6
#define BAR_GAP         1
#define MARGIN_X        8
#define NBANDS          16
#define MAX_BAR_H       90

/* Colors in RGB565 (Synthwave Theme) */
#define COL_BG          0x0000  /* Deep Black */
#define COL_HEADER_ACC  0x5D7F  /* Neon Cyan (#58a6ff -> 0x5D7F) */
#define COL_SEP         0x31A6  /* Slate Gray */
#define COL_UPPER       0xA39E  /* Vivid Synthwave Purple (#a371f7 -> 0xA39E) */
#define COL_MID         0x5D7F  /* Electric Cyan */
#define COL_LOWER       0xFD6A  /* Warm Neon Orange (#ffae57 -> 0xFD6A) */
#define COL_PEAK        0xFFFF  /* Crisp White */
#define COL_BASELINE    0x2945  /* Subtle Dark Baseline */
#define COL_TEXT        0x5D7F  /* Header Cyan Text */
#define COL_TEXT_DIM    0x8410  /* Dim Muted Gray */

/* Gradient zone thresholds */
#define ZONE_MID_Y      (BARS_TOP + 27)
#define ZONE_UPPER_Y    (BARS_TOP + 63)

#define Y_BOTTOM        (BARS_TOP + MAX_BAR_H)

/* State */
static uint8_t  rendered_h[NBANDS];   /* last drawn bar height */
static uint8_t  peak_h[NBANDS];       /* peak-hold height */
static uint8_t  peak_counter[NBANDS]; /* hold frames remaining */
static uint8_t  bands[NBANDS];        /* current FFT band heights */
static uint32_t last_update_ms;

/* Returns zone color based on Y coordinate */
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

/* Draw absolute rows [y0, y1) of one bar */
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

/* Splash and Boot Loading Animation */
static void show_boot_animation(void)
{
    int16_t bar_start_x = 20;
    int16_t bar_start_y = 75;
    int16_t bar_width = 88;
    int16_t bar_height = 6;
    int progress;

    ST7735_FillScreen(COL_BG);

    /* Draw Logo / Title */
    ST7735_DrawString(24, 45, "USB AUDIO DAC", COL_HEADER_ACC, COL_BG);

    /* Draw Progress Bar Frame */
    ST7735_DrawHLine(bar_start_x - 1, bar_start_y - 1, bar_width + 2, COL_SEP);
    ST7735_DrawHLine(bar_start_x - 1, bar_start_y + bar_height, bar_width + 2, COL_SEP);
    ST7735_DrawVLine(bar_start_x - 1, bar_start_y - 1, bar_height + 2, COL_SEP);
    ST7735_DrawVLine(bar_start_x + bar_width, bar_start_y - 1, bar_height + 2, COL_SEP);

    /* Smooth Progress Fill with Color Evolution */
    for (progress = 0; progress <= bar_width - 2; progress += 2) {
        uint16_t col;
        if (progress < (bar_width / 3)) {
            col = COL_LOWER;
        } else if (progress < (2 * bar_width / 3)) {
            col = COL_MID;
        } else {
            col = COL_UPPER;
        }

        ST7735_FillRect(bar_start_x + progress, bar_start_y, 2, bar_height, col);
        HAL_Delay(15);
    }

    HAL_Delay(200);
}

void Visualizer_Init(void)
{
    int i;

    ST7735_Init();

    /* Play smooth loading animation */
    show_boot_animation();

    /* Clear and prepare main interface */
    ST7735_FillScreen(COL_BG);

    /* Header & UI Frame */
    ST7735_DrawString(4, 5, "USB AUDIO", COL_TEXT, COL_BG);
    ST7735_DrawString(92, 5, "44.1k", COL_TEXT, COL_BG);
    ST7735_DrawHLine(0, SEP_Y, ST7735_WIDTH, COL_SEP);
    ST7735_DrawHLine(0, BASELINE_Y, ST7735_WIDTH, COL_BASELINE);

    /* Frequency Band Ticks */
    ST7735_DrawString(6, 120, "60", COL_TEXT_DIM, COL_BG);
    ST7735_DrawString(54, 120, "1k", COL_TEXT_DIM, COL_BG);
    ST7735_DrawString(104, 120, "16k", COL_TEXT_DIM, COL_BG);

    /* Reset state */
    for (i = 0; i < NBANDS; i++) {
        rendered_h[i] = 0;
        peak_h[i] = 0;
        peak_counter[i] = 0;
        bands[i] = 0;
    }
    last_update_ms = 0;
}

void Visualizer_Update(void)
{
    uint32_t now = HAL_GetTick();
    int i;

    /* Rate limit: 50 Hz update rate (20 ms frame budget) */
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

        /* Smooth Peak Ballistics with Exponential Decay */
        if (new_h >= peak_h[i]) {
            peak_h[i] = new_h;
            peak_counter[i] = 10; /* Hold peak for 10 frames (~200ms) */
        } else if (peak_counter[i] > 0U) {
            peak_counter[i]--;
        } else if (peak_h[i] > new_h) {
            /* Smooth gravity fall */
            uint8_t drop = (uint8_t)((peak_h[i] - new_h) >> 2);
            if (drop < 1U) drop = 1U;
            if (peak_h[i] > drop) {
                peak_h[i] -= drop;
            } else {
                peak_h[i] = 0;
            }
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

        /* Differential rendering of audio bar */
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

        /* Differential render of peak indicator (2-pixel solid bar to remove graininess) */
        if (old_peak > 0U && old_peak != peak_h[i]) {
            int16_t py = (int16_t)(Y_BOTTOM - old_peak);
            uint16_t restore_col = (old_peak <= new_h) ? bar_zone_color(py) : COL_BG;
            ST7735_FillRect(x, py, BAR_W, 2, restore_col);
        }

        if (peak_h[i] > 0U) {
            int16_t py = (int16_t)(Y_BOTTOM - peak_h[i]);
            ST7735_FillRect(x, py, BAR_W, 2, COL_PEAK);
        }

        rendered_h[i] = new_h;
    }

    last_update_ms = now;
}
