/* Core/Src/visualizer.c
 * 10-band rainbow LED-block spectrum visualizer (ST7735 128x128) in a
 * RescuePulse-style dark skin.
 *
 * - RescuePulse layout language: full-width dark-slate header band with
 *   centered device title, dark panel frame around the spectrum, big 2x
 *   status text on the splash, small 1x gray detail lines.
 * - Each bar is a column of stacked LED blocks (6 px block, 1 px gap).
 *   Heights and tops snap to whole blocks, so a bar always reads as
 *   discrete blocks piled on one another, never a solid rectangle.
 * - Every column gets its own color pulled from a 10-step rainbow:
 *   red -> orange -> chartreuse -> green -> spring green -> cyan ->
 *   azure -> blue-violet -> violet -> pink, left to right. Gaps and the
 *   panel interior stay pure black.
 * - Bars are smoothed (fast-but-gradual rise, relaxed fall) and peaked
 *   by a 2 px hot-pink dot that floats above the stack as it decays.
 * - Boot splash runs ~1.9 s (46 fill steps x 38 ms + READY hold) so the
 *   loading meter is readable but still under 2 s.
 * - Text uses the 8x8 row-major font (2x for splash titles).
 */

#include "visualizer.h"
#include "st7735.h"
#include "audio_fft.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

/* Layout (Max bar height is pinned to the FFT's own cap so the
 * differential renderer can never draw outside the panel). */
#define HEADER_H       16      /* full-width header band height */
#define SEP_Y          19      /* boundary below the header band */
#define PANEL_X        2       /* panel frame left  */
#define PANEL_RIGHT    125     /* panel frame right */
#define PANEL_TOP      20      /* panel frame top   */
#define BARS_TOP       24      /* first pixel row of bar area */
#define MAX_BAR_H      AUDIO_FFT_MAX_HEIGHT          /* 90 */
#define BASELINE_Y     (BARS_TOP + MAX_BAR_H)        /* 114 == panel bottom */
#define PANEL_BOTTOM   BASELINE_Y
#define Y_BOTTOM       (BARS_TOP + MAX_BAR_H)        /* 114 */
#define BAR_W          11
#define BAR_GAP        1
#define MARGIN_X       4
#define NBANDS         10

/* LED block geometry: 6 px lit block + 1 px black gap = 7 px unit.
 * 90 px column snaps to 12 full blocks (84 px), 6 px air at the top. */
#define BLOCK_H        6
#define BLOCK_GAP      1
#define BLOCK_UNIT     (BLOCK_H + BLOCK_GAP)         /* 7 */

/* Boot animation pacing: 46 steps x 38 ms ~= 1.75 s + 150 ms READY. */
#define BOOT_STEP_DELAY_MS  38
#define BOOT_HOLD_MS        150

/* Cyberpunk Neon palette (RGB565, from the design spec 4.2 + RescuePulse). */
#define COL_BG          0x0000  /* pure black                    */
#define COL_HEADER_ACC  0x073E  /* bright cyan #00E5FF           */
#define COL_SEP         0x4208  /* medium slate                  */
#define COL_PANEL       0x2104  /* dark slate header/splash panel */
#define COL_PANEL_BD    0x4208  /* panel frame                   */
#define COL_UPPER       0xF80F  /* electric magenta (splash fill) */
#define COL_MID         0x07BF  /* hyper cyan    (splash fill)    */
#define COL_LOWER       0x280C  /* deep indigo   (splash fill)    */
#define COL_PEAK        0xF950  /* hot neon pink peak dot        */
#define COL_BASELINE    0x8410  /* neutral gray baseline         */
#define COL_TEXT        0x073E  /* header / title cyan           */
#define COL_TEXT_DIM    0x7BEF  /* light gray labels             */
#define COL_OK          0x07E0  /* green "READY"                 */

/* One rainbow color per bar column, left -> right. Evenly spaced hues
 * converted to RGB565. */
static const uint16_t band_colors[NBANDS] = {
    0xF800,   /* red          */
    0xFCC0,   /* orange       */
    0xCFE0,   /* chartreuse   */
    0x37E0,   /* green        */
    0x07EC,   /* spring green */
    0x07FF,   /* cyan         */
    0x033F,   /* azure        */
    0xC81F,   /* blue-violet  */
    0x301F,   /* violet       */
    0xF80C    /* pink         */
};

/* Merge the FFT's 16 log-spaced bands down to 10 display columns so the
 * whole 172 Hz -> ~14 kHz range is still shown. Each display band uses
 * the louder of its two source bands (max keeps kick peaks). */
static const uint8_t merge_lo[NBANDS] = { 0, 1,  2,  3,  4, 6,  8, 10, 12, 14 };
static const uint8_t merge_hi[NBANDS] = { 0, 1,  2,  3,  5, 7,  9, 11, 13, 15 };

/* Bar ballistics: per-frame lerp factors. Rise is caught fast but not
 * teleported; fall relaxes for a smooth tumbling drop. */
#define VIS_ATTACK_UP   0.35f
#define VIS_ATTACK_DOWN 0.15f

/* State (bands[] must match the FFT's own 16-band width for the
 * AudioFFT_Process call; NBANDS is the on-screen column count). */
static uint8_t  rendered_h[NBANDS];       /* last drawn (snapped) height */
static uint8_t  peak_h[NBANDS];           /* peak-hold height */
static uint8_t  peak_counter[NBANDS];     /* hold frames remaining */
static uint8_t  bands[AUDIO_FFT_BANDS];   /* current FFT band heights */
static float    display_h[NBANDS];        /* smoothed bar heights */
static uint32_t last_update_ms;

/* Snap a height down to a whole-block multiple so bar tops always land
 * on a flat block edge. */
static uint8_t snap_height(uint8_t h)
{
    return (uint8_t)(((uint16_t)h / (uint16_t)BLOCK_UNIT) *
                     (uint16_t)BLOCK_UNIT);
}

/* Draw only the lit block rows of [y0, y1) for one bar; gap rows are
 * left black. Callers feed snapped spans so the region edges align to
 * block boundaries. */
static void draw_bar_blocks(int16_t x, int16_t y0, int16_t y1, uint16_t col)
{
    while (y0 < y1) {
        uint16_t rel = (uint16_t)(Y_BOTTOM - 1 - y0);
        if ((rel % (uint16_t)BLOCK_UNIT) < (uint16_t)BLOCK_H) {
            ST7735_FillRect(x, y0, BAR_W, 1, col);
        }
        y0++;
    }
}

/* Restore one row that an old peak dot may have covered: repaint it with
 * the bar color only if it is a lit block row inside the current bar;
 * background and gap rows simply stay black. */
static void restore_peak_row(int16_t x, int16_t y, uint8_t new_h, uint16_t col)
{
    int16_t  bar_top = (int16_t)(Y_BOTTOM - new_h);
    uint16_t rel;

    if (y < bar_top) {
        return; /* above the bar: pure background */
    }
    rel = (uint16_t)(Y_BOTTOM - 1 - y);
    if ((rel % (uint16_t)BLOCK_UNIT) < (uint16_t)BLOCK_H) {
        ST7735_FillRect(x, y, BAR_W, 1, col);
    }
}

/* Draw the dark panel frame around the spectrum area. */
static void draw_panel_frame(void)
{
    int16_t h = (int16_t)(PANEL_BOTTOM - PANEL_TOP + 1);

    ST7735_DrawHLine(PANEL_X,     PANEL_TOP,    (int16_t)(PANEL_RIGHT - PANEL_X + 1), COL_PANEL_BD);
    ST7735_DrawHLine(PANEL_X,     PANEL_BOTTOM, (int16_t)(PANEL_RIGHT - PANEL_X + 1), COL_PANEL_BD);
    ST7735_DrawVLine(PANEL_X,     PANEL_TOP,    h, COL_PANEL_BD);
    ST7735_DrawVLine(PANEL_RIGHT, PANEL_TOP,    h, COL_PANEL_BD);

    /* Explicitly black interior (dark theme behind the spectrum). */
    ST7735_FillRect(PANEL_X + 1, PANEL_TOP + 1,
                    (int16_t)(PANEL_RIGHT - PANEL_X - 1),
                    (int16_t)(PANEL_BOTTOM - PANEL_TOP - 1), COL_BG);
}

/* Splash and Boot Loading Animation (RescuePulse-style centered stack). */
static void show_boot_animation(void)
{
    const int16_t bar_start_x = 18;
    const int16_t bar_start_y = 80;
    const int16_t bar_width   = 92;
    const int16_t bar_height  = 7;
    int progress;

    ST7735_FillScreen(COL_BG);

    /* Centered title stack: 2x main word, 1x subtitle + spec line. */
    ST7735_DrawStringCentered(24, "AUDIO", COL_HEADER_ACC, COL_BG, 2);
    ST7735_DrawStringCentered(46, "SYNTHWAVE", COL_UPPER, COL_BG, 1);
    ST7735_DrawStringCentered(58, "44.1 kHz / I2S", COL_TEXT_DIM, COL_BG, 1);

    /* Dark panel + frame behind the progress bar. */
    ST7735_FillRect(bar_start_x - 1, bar_start_y - 1, bar_width + 2,
                    bar_height + 2, COL_PANEL);
    ST7735_DrawHLine(bar_start_x - 1, bar_start_y - 1, bar_width + 2, COL_BASELINE);
    ST7735_DrawHLine(bar_start_x - 1, bar_start_y + bar_height, bar_width + 2, COL_BASELINE);
    ST7735_DrawVLine(bar_start_x - 1, bar_start_y - 1, bar_height + 2, COL_BASELINE);
    ST7735_DrawVLine(bar_start_x + bar_width, bar_start_y - 1, bar_height + 2, COL_BASELINE);

    /* Slow Neon Color Evolution Fill (~1.75 s -> total boot ~1.9 s). */
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
        HAL_Delay(BOOT_STEP_DELAY_MS);
    }

    ST7735_DrawStringCentered(100, "READY", COL_OK, COL_BG, 2);
    HAL_Delay(BOOT_HOLD_MS);
}

void Visualizer_Init(void)
{
    int i;

    ST7735_Init();

    /* Play smooth loading animation */
    show_boot_animation();

    /* Clear and prepare main interface */
    ST7735_FillScreen(COL_BG);

    /* RescuePulse-style header band with centered device title. */
    ST7735_FillRect(0, 0, ST7735_WIDTH, HEADER_H, COL_PANEL);
    ST7735_DrawString(4, 4, "USB AUDIO", COL_TEXT, COL_PANEL);
    ST7735_DrawString(84, 4, "44.1k", COL_TEXT_DIM, COL_PANEL);
    ST7735_DrawHLine(0, SEP_Y, ST7735_WIDTH, COL_SEP);

    /* Spectrum panel: dark frame, black interior, baseline, freq labels. */
    draw_panel_frame();
    ST7735_DrawString(6, 120, "60", COL_TEXT_DIM, COL_BG);
    ST7735_DrawString(56, 120, "1k", COL_TEXT_DIM, COL_BG);
    ST7735_DrawString(104, 120, "16k", COL_TEXT_DIM, COL_BG);

    /* Reset state */
    for (i = 0; i < NBANDS; i++) {
        rendered_h[i] = 0;
        peak_h[i] = 0;
        peak_counter[i] = 0;
        display_h[i] = 0.0f;
    }
    for (i = 0; i < AUDIO_FFT_BANDS; i++) {
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
        uint16_t col = band_colors[i];
        uint8_t old_h = rendered_h[i];
        uint8_t old_peak = peak_h[i];
        float   tgt;
        uint8_t raw_new;
        uint8_t new_h;

        /* Merge the two source FFT bands, keeping the louder one. */
        tgt = (float)bands[merge_lo[i]];
        if ((float)bands[merge_hi[i]] > tgt) {
            tgt = (float)bands[merge_hi[i]];
        }

        /* Smooth bar ballistics so heights glide instead of snapping. */
        if (tgt > (float)AUDIO_FFT_MAX_HEIGHT) {
            tgt = (float)AUDIO_FFT_MAX_HEIGHT;
        }
        if (tgt > display_h[i]) {
            display_h[i] += (tgt - display_h[i]) * VIS_ATTACK_UP;
        } else {
            display_h[i] += (tgt - display_h[i]) * VIS_ATTACK_DOWN;
        }
        raw_new = (uint8_t)(display_h[i] + 0.5f);
        if (raw_new > AUDIO_FFT_MAX_HEIGHT) {
            raw_new = AUDIO_FFT_MAX_HEIGHT;
        }
        new_h = snap_height(raw_new);

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

        /* Differential rendering of the LED block bar */
        if (new_h > old_h) {
            draw_bar_blocks(x,
                            (int16_t)(Y_BOTTOM - new_h),
                            (int16_t)(Y_BOTTOM - old_h),
                            col);
        } else if (new_h < old_h) {
            /* Shrinking: everything in the span becomes black again
             * (gaps are already black; a flat fill erases leftover
             * block pixels too). */
            ST7735_FillRect(x,
                            (int16_t)(Y_BOTTOM - old_h),
                            BAR_W,
                            (int16_t)(old_h - new_h),
                            COL_BG);
        }

        /* Differential render of the 2 px peak mark. Restore each old
         * row to bar color only where a lit block actually belongs, so
         * no ghost pixels and no filled gaps. */
        if (old_peak > 0U && old_peak != peak_h[i]) {
            int16_t row0 = (int16_t)(Y_BOTTOM - old_peak);

            restore_peak_row(x, row0, new_h, col);
            restore_peak_row(x, row0 + 1, new_h, col);
        }

        if (peak_h[i] > 0U) {
            int16_t py = (int16_t)(Y_BOTTOM - peak_h[i]);
            ST7735_FillRect(x, py, BAR_W, 2, COL_PEAK);
        }

        rendered_h[i] = new_h;
    }

    last_update_ms = now;
}