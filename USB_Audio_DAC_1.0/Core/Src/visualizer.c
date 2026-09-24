/* Core/Src/visualizer.c
 * 16-band differential spectrum visualizer, Cyberpunk Neon theme
 * (ST7735 128x128).
 *
 * - Palette follows the design spec: electric magenta / hyper cyan /
 *   deep synthwave indigo, hot-pink peak dots (not blue/green/yellow).
 * - Bars are smoothed in the visualizer (fast-but-gradual rise,
 *   relaxed fall) so they no longer snap at the FFT frame rate.
 * - Peak dots are 2 px hot pink; restoring an old dot clears BOTH rows
 *   with the correct per-row zone color, so no colored ghosts remain.
 * - Boot animation uses the 2x scaled font for a legible title.
 */

#include "visualizer.h"
#include "st7735.h"
#include "audio_fft.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

/* Layout */
#define SEP_Y           19
#define BARS_TOP        20      /* first pixel row of bar area */
#define BASELINE_Y      119
#define BAR_W           6
#define BAR_GAP         1
#define MARGIN_X        8
#define NBANDS          16
#define MAX_BAR_H       90

/* Cyberpunk Neon palette (RGB565, from the design spec §4.2). */
#define COL_BG          0x0000  /* pure black                    */
#define COL_HEADER_ACC  0x073E  /* bright cyan #00E5FF           */
#define COL_SEP         0x4208  /* medium slate                  */
#define COL_UPPER       0xF80F  /* electric magenta (top 30%)    */
#define COL_MID         0x07BF  /* hyper cyan    (mid 40%)       */
#define COL_LOWER       0x280C  /* deep indigo   (base 30%)      */
#define COL_PEAK        0xF950  /* hot neon pink peak dot        */
#define COL_BASELINE    0x8410  /* neutral gray baseline         */
#define COL_TEXT        0x073E  /* header / title cyan           */
#define COL_TEXT_DIM    0x7BEF  /* light gray labels             */

/* Gradient zone thresholds */
#define ZONE_MID_Y      (BARS_TOP + 27)
#define ZONE_UPPER_Y    (BARS_TOP + 63)

#define Y_BOTTOM        (BARS_TOP + MAX_BAR_H)

/* Bar ballistics: per-frame lerp factors. Rise is caught fast but not
 * teleported; fall relaxes for a smooth tumbling drop. */
#define VIS_ATTACK_UP   0.35f
#define VIS_ATTACK_DOWN 0.15f

/* State */
static uint8_t  rendered_h[NBANDS];   /* last drawn bar height */
static uint8_t  peak_h[NBANDS];       /* peak-hold height */
static uint8_t  peak_counter[NBANDS]; /* hold frames remaining */
static uint8_t  bands[NBANDS];        /* current FFT band heights */
static float    display_h[NBANDS];    /* smoothed bar heights */
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
    const int16_t bar_start_x = 18;
    const int16_t bar_start_y = 82;
    const int16_t bar_width   = 92;
    const int16_t bar_height  = 7;
    int progress;

    ST7735_FillScreen(COL_BG);

    /* Legible 2x title (12x16 px per glyph). */
    ST7735_DrawString2x(10, 30, "USB AUDIO", COL_HEADER_ACC, COL_BG);
    ST7735_DrawString(37, 54, "SYNTHWAVE", COL_UPPER, COL_BG);

    /* Draw Progress Bar Frame */
    ST7735_DrawHLine(bar_start_x - 1, bar_start_y - 1, bar_width + 2, COL_BASELINE);
    ST7735_DrawHLine(bar_start_x - 1, bar_start_y + bar_height, bar_width + 2, COL_BASELINE);
    ST7735_DrawVLine(bar_start_x - 1, bar_start_y - 1, bar_height + 2, COL_BASELINE);
    ST7735_DrawVLine(bar_start_x + bar_width, bar_start_y - 1, bar_height + 2, COL_BASELINE);

    /* Smooth Progress Fill with Neon Color Evolution */
    for (progress = 0; progress <= bar_width - 2; progress += 2) {
        uint16_t col;

        if (progress < (bar_width / 3)) {
            col = COL_LOWER;      /* indigo  */
        } else if (progress < (2 * bar_width / 3)) {
            col = COL_MID;        /* cyan    */
        } else {
            col = COL_UPPER;      /* magenta */
        }

        ST7735_FillRect(bar_start_x + progress, bar_start_y, 2, bar_height, col);
        HAL_Delay(9);
    }

    ST7735_DrawString(44, 100, "READY", COL_MID, COL_BG);
    HAL_Delay(150);
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
    ST7735_DrawString(92, 5, "44.1k", COL_TEXT_DIM, COL_BG);
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
        display_h[i] = 0.0f;
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
        float   tgt = (float)bands[i];
        uint8_t new_h;

        /* Smooth bar ballistics so heights glide instead of snapping. */
        if (tgt > (float)AUDIO_FFT_MAX_HEIGHT) {
            tgt = (float)AUDIO_FFT_MAX_HEIGHT;
        }
        if (tgt > display_h[i]) {
            display_h[i] += (tgt - display_h[i]) * VIS_ATTACK_UP;
        } else {
            display_h[i] += (tgt - display_h[i]) * VIS_ATTACK_DOWN;
        }
        new_h = (uint8_t)(display_h[i] + 0.5f);
        if (new_h > AUDIO_FFT_MAX_HEIGHT) {
            new_h = AUDIO_FFT_MAX_HEIGHT;
        }

        /* Smooth Peak Ballistics with Exponential Decay */
        if (new_h >= peak_h[i]) {
            peak_h[i] = new_h;
            peak_counter[i] = 12; /* Hold peak for 12 frames (~240ms) */
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

        /* Differential render of the 2 px peak mark. Clear each of the
         * two old rows with the color that actually belongs there (bar
         * zone color per row, or background), so no ghost pixels remain. */
        if (old_peak > 0U && old_peak != peak_h[i]) {
            int16_t row0 = (int16_t)(Y_BOTTOM - old_peak);
            int16_t bar_top = (int16_t)(Y_BOTTOM - new_h);

            if (row0 >= bar_top) {
                ST7735_FillRect(x, row0, BAR_W, 1, bar_zone_color(row0));
                ST7735_FillRect(x, row0 + 1, BAR_W, 1, bar_zone_color(row0 + 1));
            } else {
                ST7735_FillRect(x, row0, BAR_W, 2, COL_BG);
            }
        }

        if (peak_h[i] > 0U) {
            int16_t py = (int16_t)(Y_BOTTOM - peak_h[i]);
            ST7735_FillRect(x, py, BAR_W, 2, COL_PEAK);
        }

        rendered_h[i] = new_h;
    }

    last_update_ms = now;
}
