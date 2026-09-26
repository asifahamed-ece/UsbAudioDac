/* Core/Src/visualizer.c
 * 12-band rainbow LED-block spectrum visualizer (ST7735 128x128) in a
 * RescuePulse-style dark skin.
 *
 * - RescuePulse layout language: full-width dark-slate header band with
 *   the device name left-anchored and the rate stamp right-anchored
 *   against the same margin, a dark panel frame around the spectrum, and
 *   small 1x gray detail lines. Only the splash's "AUDIO" word is drawn
 *   at 2x; every other string on the device is 1x.
 * - Each bar is a column of stacked LED blocks (6 px block, 1 px gap).
 *   Heights and tops snap to whole blocks, so a bar always reads as
 *   discrete blocks piled on one another, never a solid rectangle.
 * - Every column gets its own color from a 12-entry rainbow:
 *   red -> orange -> chartreuse -> green -> spring green -> cyan ->
 *   azure -> blue-violet -> violet -> pink -> magenta -> neon pink, left
 *   to right. Gaps and the panel interior stay pure black.
 * - Bars are smoothed (fast-but-gradual rise, relaxed fall) and peaked
 *   by a 2 px hot-pink dot that floats above the stack as it decays.
 * - Boot splash runs ~3.5 s (54 ticks x 55 ms + a 500 ms READY hold).
 * - Text uses the single 8x8 row-major font in font8x8.h; there is no
 *   second font table in the project.
 */

#include "visualizer.h"
#include "st7735.h"
#include "audio_fft.h"
#include "usbd_conf.h"      /* USBD_AUDIO_FREQ — the one source of truth for the rate */
#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <string.h>         /* strlen, for right-aligning the rate stamp */

/* Layout (Max bar height is pinned to the FFT's own cap so the
 * differential renderer can never draw outside the panel). */
#define HEADER_H       16      /* full-width header band height */
#define SEP_Y          19      /* boundary below the header band */
#define PANEL_X        2       /* panel frame left  */
#define PANEL_RIGHT    125     /* panel frame right */
#define PANEL_TOP      20      /* panel frame top   */
#define BARS_TOP       24      /* first pixel row of bar area */
#define MAX_BAR_H      AUDIO_FFT_MAX_HEIGHT          /* 84 = 12 x 7 blocks */
#define BASELINE_Y     (BARS_TOP + MAX_BAR_H)        /* 108 == panel bottom */
#define PANEL_BOTTOM   BASELINE_Y
#define Y_BOTTOM       (BARS_TOP + MAX_BAR_H)        /* 108 */
#define BAR_W          9
#define BAR_GAP        1
#define MARGIN_X       4
#define NBANDS         12

/* Header band text. The device name is left-aligned; the rate stamp is
 * right-aligned against the same 4 px margin, so the band reads as two
 * anchored ends rather than one centred blob. */
#define HEADER_TEXT_Y  4
#define HEADER_PAD     4      /* left margin for the name, right for the stamp */
#define RATE_STAMP_MAX_CHARS 4 /* worst case is 3 digits + 'k', e.g. "192k" */

/* Compile-time guard: the two header strings must never overlap, even at
 * the longest stamp the builder can produce. "SYNTHWAVE" is 9 x 8 = 72 px,
 * so it ends at 76; the widest stamp starts at 128 - 4 - 32 = 92. */
typedef char header_texts_must_not_collide[
    ((HEADER_PAD + (9 * 8)) < (ST7735_WIDTH - HEADER_PAD - (RATE_STAMP_MAX_CHARS * 8))) ? 1 : -1];

/* Header rate stamp, built from USBD_AUDIO_FREQ at init.
 *
 * Deliberately NOT a string literal, and deliberately NOT stringified with
 * the preprocessor. This label has failed both ways already: it read
 * "44.1k" after the device moved to 48 kHz, and the fix that tried to
 * derive it printed the literal text "48000U / 1000", because `#` does not
 * macro-expand its argument. Converting the digits at runtime sidesteps
 * both failure modes -- it cannot go stale, and it cannot print an
 * expression. */
#define RATE_STAMP_BUF 6      /* "192k" + NUL */
static char rate_stamp[RATE_STAMP_BUF];

static void build_rate_stamp(void)
{
    uint32_t k = USBD_AUDIO_FREQ / 1000U;   /* 48000 -> 48 */
    char rev[4];
    int n = 0;
    int i;

    /* Pull decimal digits least-significant first, then reverse. */
    do {
        rev[n] = (char)('0' + (k % 10U));
        n++;
        k /= 10U;
    } while ((k != 0U) && (n < 3));

    for (i = 0; i < n; i++) {
        rate_stamp[i] = rev[n - 1 - i];
    }
    rate_stamp[n]     = 'k';
    rate_stamp[n + 1] = '\0';
}

/* Region label strip.
 *
 * The panel's last 3 logical rows (125..127) are written to GRAM rows
 * 128..130 through ST7735_ROWSTART and are NOT visible on the glass, so
 * anything drawn there is silently half-cut. The old labels sat at y=120
 * (rows 120..127) and were clipped by exactly that. The strip is placed
 * so the whole 8-row glyph ends well clear of them.
 *
 * MAX_BAR_H was reduced 90 -> 84 to make room: 84 is exactly 12 LED
 * blocks of 7 px, which also removes the 6 px "air" row the old 90 px
 * stack left at the top of a full column.
 *
 * Text uses font8x8.h via ST7735_DrawString -- the same table that
 * already renders correctly on this panel. A separate 5x7 table was tried
 * here and came out transposed on the glass, so there is exactly one font
 * in the project now. */
#define LABEL_SEP_Y    110     /* 1 px divider between panel and labels */
#define LABEL_Y        112     /* rows 112..119, clear of the clipped 125..127 */
#define LABEL_MAX_Y    (LABEL_Y + 8 - 1)            /* 119 */

/* Compile-time guard: fail the build rather than ship a clipped label
 * strip. A negative array size is a constraint violation, i.e. an error.
 * This is the regression that put the old "Lows/Mids/Highs" text at y=120,
 * where its bottom rows were written to GRAM rows the panel never shows. */
typedef char label_strip_must_fit[(LABEL_MAX_Y < ST7735_USABLE_BOTTOM) ? 1 : -1];

/* Label x positions, centred under the band groups the FFT actually
 * produces (2 bass / 6 mid / 4 high at 48 kHz: 47-328, 328-4406,
 * 4406-13172 Hz). Bar i spans x = MARGIN_X + i*(BAR_W+BAR_GAP) and is
 * BAR_W wide, so its centre is 8 + i*10.
 *   bass  bands 0-1  -> centres 8,18    -> group centre 13
 *   mid   bands 2-7  -> centres 28..78  -> group centre 53
 *   high  bands 8-11 -> centres 88..118 -> group centre 103
 * An 8x8 glyph advances 8 px, so 3 characters occupy 24 px. */
#define LABEL_LO_X     1       /* 3 chars = 24 px, centred on 13 */
#define LABEL_MID_X    41      /* 3 chars = 24 px, centred on 53 */
#define LABEL_HI_X     95      /* 2 chars = 16 px, centred on 103 */

/* LED block geometry: 6 px lit block + 1 px black gap = 7 px unit.
 * MAX_BAR_H is 84 = 12 whole blocks, so a full column is exactly 12 blocks
 * with no partial "air" row at the top (the old 90 px stack left 6 px of
 * dead space above the twelfth block). */
#define BLOCK_H        6
#define BLOCK_GAP      1
#define BLOCK_UNIT     (BLOCK_H + BLOCK_GAP)         /* 7 */
#define BLOCKS_FULL    (MAX_BAR_H / BLOCK_UNIT)      /* 12 */

/* Boot animation pacing.
 *
 * HARD BUDGET: Visualizer_Init() runs after MX_USB_DEVICE_Init(), so a slow
 * boot eats into the host's enumeration window. There is a reverted commit
 * in this repo's history ("move Visualizer_Init after USB enumeration to
 * avoid host timeout"), which is exactly this failure.
 *
 * Budget with the current numbers: backdrop ~32 ms + 54 ticks x 55 ms
 * = 2.97 s + 500 ms READY hold ~= 3.5 s, inside the 5 s the owner signed
 * off on. If enumeration ever gets flaky, BOOT_STEP_DELAY_MS is the dial.
 *
 * Per-tick cost is deliberately O(one grid line + one short string), not
 * O(full screen): a full 128x128 repaint is 32768 bytes ~= 21.8 ms at the
 * 12 MHz SPI1 clock, which would blow the whole budget in ten frames.
 */
#define BOOT_TICKS              54
#define BOOT_STEP_DELAY_MS      55
#define BOOT_HOLD_MS            500

/* Scene geometry — one continuous synthwave shot: title stack and a setting
 * sun in the sky, a bright horizon, a grid floor running unbroken from that
 * horizon down, and the loading controls anchored at the very bottom.
 *
 * Every element is placed to stay inside ST7735_USABLE_BOTTOM (125); the last
 * 3 rows are never displayed (see ST7735_HIDDEN_BOTTOM_ROWS).
 *
 * Sky ....... rows 3..37   title stack
 * Sun ....... rows 41..68  28 px disc, centre row 55
 * Horizon ... row 58       cuts the disc: 11 of its 28 rows sit below
 * Ground .... rows 59..98  black cover hides the submerged disc, then the
 *                           grid floor; the rays converge at (64, 58)
 * Controls .. rows 100..117
 *
 * The disc's widest point is row 55, three rows ABOVE the horizon, so the sun
 * reads as *setting* rather than sinking. */
#define SUN_TOP          41
#define SUN_R            28     /* diameter; r = 14, so x spans 50..78   */
#define SUN_CX           64
#define SUN_BAND_H       2      /* lit rows per band                    */
#define SUN_BAND_GAP     2      /* dark rows between bands              */
#define HORIZON_Y        58     /* bright line; also the vanishing point */
#define GROUND_TOP       59     /* black fill hides the submerged disc  */
#define TITLE_Y          3      /* "AUDIO" at 2x -> rows 3..18          */
#define SUBTITLE_Y       21     /* "SYNTHWAVE" 8x8 -> rows 21..28       */
#define RATE_Y           30     /* "USB AUDIO DAC" -> rows 30..37       */
#define GRID_TOP         71
#define GRID_BOTTOM      98
#define GRID_ROWS        6      /* horizontal lines, both endpoints incl. */
#define GRID_RAYS        4      /* +/- this many rays either side of centre */
#define BOOT_BAR_X       22
#define BOOT_BAR_Y       101    /* rows 101..106, frame 100..107 */
#define BOOT_BAR_W       84
#define BOOT_BAR_H       6
#define BOOT_READY_Y     110    /* rows 110..117 */

/* Compile-time guard: the status line must clear the 3 unreachable bottom
 * rows. A negative array size is a constraint violation, i.e. an error. */
typedef char boot_status_must_fit[(BOOT_READY_Y + 7 < ST7735_USABLE_BOTTOM) ? 1 : -1];

/* Boot phases. These are the words the splash actually shows, in order.
 *
 * The animation runs BOOT_TICKS ticks, but only changes word every
 * BOOT_WORD_TICKS of them, so each word stays on screen long enough to be
 * read instead of strobing past. A 27-tick run changed the word on almost
 * every tick, which is both unreadable and what made the stale-edge bug so
 * obvious. */
static const char *const boot_status[] = {
    "CLOCKS", "PLL", "I2S", "DMA", "USB FS", "RING", "TFT", "MIX",
    "OUT", "CHECK", "READY"
};
#define BOOT_WORDS  ((int32_t)(sizeof(boot_status) / sizeof(boot_status[0])))
/* Ticks each word is held. Derived, so words and ticks cannot disagree. */
#define BOOT_WORD_TICKS  ((int32_t)((BOOT_TICKS + BOOT_WORDS - 1) / BOOT_WORDS))

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

/* Retro-sun scene palette. The sun is banded like a synthwave sunset:
 * hot magenta at the top through orange to gold at the horizon. */
#define COL_SUN_TOP     0xF80F  /* hot magenta band               */
#define COL_SUN_MID     0xFD20  /* orange band                    */
#define COL_SUN_LOW     0xFFE0  /* gold band                      */
#define COL_YELLOW      0xFFE0  /* header rate stamp (same gold)  */
#define COL_GRID        0x033F  /* azure grid lines (leading)     */
#define COL_GRID_MID    0x0228  /* mid grid lines                 */
#define COL_GRID_FAR    0x0208  /* dimmer converging lines       */
#define COL_DIM         0x4208  /* status text while pending      */

/* One rainbow color per bar column, left -> right. Evenly spaced hues
 * converted to RGB565. */
static const uint16_t band_colors[NBANDS] = {
    0xF800,   /* red            */
    0xFCC0,   /* orange         */
    0xCFE0,   /* chartreuse     */
    0x37E0,   /* green          */
    0x07EC,   /* spring green   */
    0x07FF,   /* cyan           */
    0x033F,   /* azure          */
    0xC81F,   /* blue-violet    */
    0x301F,   /* violet         */
    0xF80C,   /* pink           */
    0xF80F,   /* magenta        */
    0xF950    /* neon pink      */
};

/* The FFT now emits exactly the 12 on-screen perceptual columns, so the
 * merge is a 1:1 passthrough (kept as a table so band pairing stays
 * trivial to change later). Left = bass, right = highs. */
static const uint8_t merge_lo[NBANDS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t merge_hi[NBANDS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

/* Bar ballistics: per-frame lerp factors. Rise is caught fast but not
 * teleported; fall relaxes for a smooth tumbling drop. */
#define VIS_ATTACK_UP   0.35f
#define VIS_ATTACK_DOWN 0.15f

/* State (bands[] is the FFT's 12 perceptual bands — a 1:1 match with
 * the NBANDS screen columns, so no down-sampling happens). */
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

/* Restore one row that an old peak dot may have covered. The row must
 * always be overwritten: lit block rows inside the current bar get the
 * bar color, while gap rows, rows above the bar, and rows below the
 * baseline all go black — otherwise the falling peak dot strands pink
 * pixels behind it (stale magenta trails after pause). */
static void restore_peak_row(int16_t x, int16_t y, uint8_t new_h, uint16_t col)
{
    int16_t bar_top = (int16_t)(Y_BOTTOM - new_h);
    int16_t rel     = (int16_t)(Y_BOTTOM - 1 - y);

    if (y >= bar_top && (rel % (int16_t)BLOCK_UNIT) < (int16_t)BLOCK_H) {
        ST7735_FillRect(x, y, BAR_W, 1, col);
    } else {
        ST7735_FillRect(x, y, BAR_W, 1, COL_BG);
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

/* ============================================================================
 * BOOT SPLASH — retro sun + perspective grid
 * ============================================================================
 * Deliberately built from row-level primitives only (FillRect / DrawHLine),
 * with no framebuffer and no new driver primitives, because SPI1 runs at
 * 12 MHz: one full 128x128 repaint is 32768 B ~= 21.8 ms. Everything that
 * stays still is painted once in draw_boot_backdrop(); each animation step
 * then repaints the whole 6-line grid (~2 KB ~= 1.4 ms) plus a short status
 * string, so a full-screen repaint never appears inside the loop. Total
 * splash cost is ~3.5 s of wall clock, 99 % of which is the deliberate
 * BOOT_STEP_DELAY_MS pacing rather than drawing.
 *
 * A filled disc needs no driver support either: each row's half-width solves
 * hw^2 + dy^2 <= r^2, so the sun is just a run of FillRect calls.
 * ==========================================================================*/

/* Band colour for a sun row, ramped across the VISIBLE part of the disc.
 *
 * The ramp is keyed to HORIZON_Y, not to the disc's full height. With the
 * sun setting, 11 of its 28 rows sit below the horizon and are painted over
 * by the ground cover -- so a ramp measured over the whole disc put the warm
 * gold band at depths 26..27, i.e. entirely underwater, and the sun came out
 * with only two colours. Splitting the visible height into thirds puts the
 * gold right at the horizon, which is where a sunset is brightest. */
static uint16_t sun_color_for_row(int16_t y)
{
    int16_t depth = (int16_t)(y - SUN_TOP);
    int16_t vis   = (int16_t)(HORIZON_Y - SUN_TOP);   /* rows above the horizon */

    if ((vis <= 0) || (depth < 0)) {
        return COL_SUN_TOP;
    }
    if ((depth * 3) < vis) {
        return COL_SUN_TOP;        /* top third:    hot magenta */
    }
    if ((depth * 3) < (vis * 2)) {
        return COL_SUN_MID;        /* middle third: orange     */
    }
    return COL_SUN_LOW;            /* bottom third: gold       */
}

/* Repaint every horizontal grid line for the current animation phase.
 *
 * The whole grid is repainted each step rather than incrementally erased:
 * an earlier version advanced one row per step and erased the row behind
 * it, which ran off the bottom of the band and started deleting the
 * remaining rows (the grid visibly emptied out halfway through the
 * splash). Repainting all GRID_ROWS (6) lines is ~1.5 KB ~= 1 ms at
 * 12 MHz, so the correct version is also the cheap one.
 *
 * The motion is a highlight sweeping down the grid: each line is tinted by
 * how far behind the travelling front it is, which reads as the floor
 * scrolling toward the viewer.
 */
static void draw_boot_grid(int16_t phase)
{
    int16_t i;
    int16_t dx;

    for (i = 0; i < GRID_ROWS; i++) {
        int16_t y  = (int16_t)(GRID_TOP +
                               ((i * (GRID_BOTTOM - GRID_TOP)) / (GRID_ROWS - 1)));
        int16_t d  = (int16_t)((i - phase + (GRID_ROWS * 2)) % GRID_ROWS);
        uint16_t col = COL_GRID_FAR;

        if (d <= 1) {
            col = COL_GRID;          /* at the front: brightest */
        } else if (d <= 3) {
            col = COL_GRID_MID;
        }

        /* Leave a small gap at the centre so the converging verticals stay
         * legible through the horizontals. */
        for (dx = 0; dx < (SUN_CX - 3); dx++) {
            ST7735_DrawPixel(dx, y, col);
            ST7735_DrawPixel((int16_t)(ST7735_WIDTH - 1 - dx), y, col);
        }
    }
}

/* Paint the banded sun disc. Every SUN_BAND_GAP-row cut is skipped,
 * leaving black stripes across the disc -- banding confined to the disc is
 * deliberate: an earlier version painted dark scanlines across the full
 * screen width, which tinted the black sky navy and turned the whole
 * upper half into a striped rectangle. */
static void draw_boot_sun(void)
{
    int16_t y;
    int32_t r  = (int32_t)(SUN_R / 2);
    int32_t cy = (int32_t)(SUN_TOP + (SUN_R / 2));
    int32_t rr = r * r;

    for (y = SUN_TOP; y < (SUN_TOP + SUN_R); y++) {
        int32_t dy = (int32_t)y - cy;
        int32_t d2 = dy * dy;
        int32_t hw = 0;
        int16_t half;
        uint16_t col;

        if (d2 > rr) {
            continue;                      /* outside the disc entirely */
        }

        /* Exact half-width: the widest hw with hw^2 + dy^2 <= r^2, found
         * by walking x outward. At most r (14) iterations per row, so the
         * whole disc costs a few hundred adds and no division at all.
         *
         * Do NOT replace this with an integer Newton sqrt
         * (sqrt(x) ~= (b + x/b)/2): at the disc's poles x is 0, so x/b is
         * 0 and the iteration collapses b toward 0 instead of settling,
         * eating the top of the disc. That is what drew a cone. */
        while (((hw + 1) * (hw + 1) + d2) <= rr) {
            hw++;
        }
        half = (int16_t)hw;
        if (half <= 0) {
            continue;
        }

        /* Skip the dark gaps between bands entirely rather than drawing
         * then erasing. SUN_BAND_H lit rows, SUN_BAND_GAP dark. */
        if (((y - SUN_TOP) % (SUN_BAND_H + SUN_BAND_GAP)) >= SUN_BAND_H) {
            continue;
        }

        col = sun_color_for_row(y);
        ST7735_FillRect((int16_t)(SUN_CX - half), y, (int16_t)(half * 2), 1, col);
    }
}

/* Integer line (Bresenham) from (x0,y0) to (x1,y1), inclusive.
 *
 * The converging grid lines used to be walked with integer-percent
 * quantisation (t = vx*100/steps, then x interpolated on t). On the outer
 * rays that advances x by 2 or more per row, which leaves diagonal gaps
 * and reads as a dotted line rather than a solid one. Bresenham is
 * 4-connected, so the ray is unbroken. */
static void draw_boot_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                           uint16_t col)
{
    int16_t dx = (int16_t)((x1 > x0) ? (x1 - x0) : (x0 - x1));
    int16_t dy = (int16_t)((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int16_t sx = (int16_t)((x0 < x1) ? 1 : -1);
    int16_t sy = (int16_t)((y0 < y1) ? 1 : -1);
    int16_t err = (int16_t)(dx - dy);

    for (;;) {
        ST7735_DrawPixel(x0, y0, col);
        if ((x0 == x1) && (y0 == y1)) {
            break;
        }
        {
            int16_t e2 = (int16_t)(err * 2);
            if (e2 > -dy) { err = (int16_t)(err - dy); x0 = (int16_t)(x0 + sx); }
            if (e2 <  dx) { err = (int16_t)(err + dx); y0 = (int16_t)(y0 + sy); }
        }
    }
}

/* Static half of the scene: sky, setting sun, horizon, ground cover, grid,
 * converging rays, title stack, progress frame and the READY placeholder.
 *
 * Draw order matters. The disc is drawn whole, then the ground is painted
 * black OVER its lower part, and only then is the horizon line drawn -- so
 * the disc appears to be sinking behind the horizon instead of floating
 * above it. Drawing the horizon first would let the submerged part show. */
static void draw_boot_backdrop(void)
{
    int16_t i;

    ST7735_FillScreen(COL_BG);

    /* The whole disc first, including the part that will be submerged. */
    draw_boot_sun();

    /* Ground cover: black from just under the horizon down to the top of the
     * grid. This hides the submerged part of the disc and gives the rays a
     * black ground to run over. 128 x 12 px = 3072 B ~= 2.0 ms at 12 MHz. */
    ST7735_FillRect(0, GROUND_TOP, ST7735_WIDTH,
                    (int16_t)(GRID_TOP - GROUND_TOP), COL_BG);

    /* The horizon line, after the cover so it stays in front. No glow row
     * above it: at HORIZON_Y-1 that would stripe the disc's lower bands, the
     * same mistake the screen-wide scanlines were. */
    ST7735_FillRect(0, HORIZON_Y, ST7735_WIDTH, 1, COL_SUN_LOW);

    /* Converging rays, all meeting at the vanishing point on the horizon
     * centre. That convergence is what sells the perspective. */
    for (i = -GRID_RAYS; i <= GRID_RAYS; i++) {
        int16_t x_bottom = (int16_t)(SUN_CX + (i * (ST7735_WIDTH / 12)));

        if ((x_bottom < 0) || (x_bottom >= ST7735_WIDTH)) {
            continue;
        }
        draw_boot_line(SUN_CX, HORIZON_Y, x_bottom, GRID_BOTTOM, COL_GRID);
        draw_boot_line(SUN_CX, HORIZON_Y, x_bottom, HORIZON_Y + 7, COL_GRID_FAR);
    }

    /* Paint the grid's leading edge so it is never empty on frame 0. */
    draw_boot_grid(0);

    /* Title stack, centred above the sun. */
    ST7735_DrawStringCentered(TITLE_Y, "AUDIO", COL_HEADER_ACC, COL_BG, 2);
    ST7735_DrawStringCentered(SUBTITLE_Y, "SYNTHWAVE", COL_UPPER, COL_BG, 1);
    ST7735_DrawStringCentered(RATE_Y, "USB AUDIO DAC", COL_TEXT_DIM, COL_BG, 1);

    /* Progress frame (filled in during the animation). */
    ST7735_FillRect(BOOT_BAR_X - 1, BOOT_BAR_Y - 1, BOOT_BAR_W + 2,
                    BOOT_BAR_H + 2, COL_PANEL);
    ST7735_DrawHLine(BOOT_BAR_X - 1, BOOT_BAR_Y - 1, BOOT_BAR_W + 2, COL_BASELINE);
    ST7735_DrawHLine(BOOT_BAR_X - 1, BOOT_BAR_Y + BOOT_BAR_H, BOOT_BAR_W + 2, COL_BASELINE);
    ST7735_DrawVLine(BOOT_BAR_X - 1, BOOT_BAR_Y - 1, BOOT_BAR_H + 2, COL_BASELINE);
    ST7735_DrawVLine(BOOT_BAR_X + BOOT_BAR_W, BOOT_BAR_Y - 1, BOOT_BAR_H + 2, COL_BASELINE);

    /* READY, shown dimmed until the animation completes. */
    ST7735_DrawStringCentered(BOOT_READY_Y, "READY", COL_DIM, COL_BG, 1);
}

/* Boot splash: static backdrop once, then BOOT_TICKS cheap frames. */
static void show_boot_animation(void)
{
    int32_t tick;
    int32_t shown = -1;   /* index of the word currently on the glass */
    const char *last = NULL;

    draw_boot_backdrop();

    for (tick = 0; tick < BOOT_TICKS; tick++) {
        int32_t word  = tick / BOOT_WORD_TICKS;
        int16_t filled;
        uint16_t col;

        if (word >= BOOT_WORDS) {
            word = BOOT_WORDS - 1;
        }

        /* Advance the grid's travelling highlight (~1 ms). */
        draw_boot_grid((int16_t)(tick % GRID_ROWS));

        /* Status line. Erase the whole band before drawing, and only
         * redraw when the word actually changes. Consecutive words have
         * different lengths ("CLOCKS" is 6, "DMA" is 3), so centring a
         * short word over a long one leaves the long one's end pixels
         * behind -- measured at up to 30 stray pixels per transition,
         * which is what made the line unreadable. The erase is what
         * fixes it; skipping unchanged words is just less SPI traffic. */
        if (boot_status[word] != last) {
            ST7735_FillRect(0, BOOT_READY_Y, ST7735_WIDTH, 8, COL_BG);
            ST7735_DrawStringCentered(BOOT_READY_Y, boot_status[word],
                                      COL_TEXT_DIM, COL_BG, 1);
            last = boot_status[word];
            shown = word;
        }
        (void)shown;

        /* Progress fill, colour-evolving like the old splash. */
        filled = (int16_t)((tick * (BOOT_BAR_W - 2)) / (BOOT_TICKS - 1));
        if (filled < (BOOT_BAR_W / 3)) {
            col = COL_LOWER;
        } else if (filled < (2 * BOOT_BAR_W / 3)) {
            col = COL_MID;
        } else {
            col = COL_UPPER;
        }
        ST7735_FillRect(BOOT_BAR_X, BOOT_BAR_Y, filled, BOOT_BAR_H, col);

        HAL_Delay(BOOT_STEP_DELAY_MS);
    }

    /* Final state: solid bar, green READY. */
    ST7735_FillRect(BOOT_BAR_X, BOOT_BAR_Y, BOOT_BAR_W, BOOT_BAR_H, COL_OK);
    ST7735_FillRect(0, BOOT_READY_Y, ST7735_WIDTH, 8, COL_BG);
    ST7735_DrawStringCentered(BOOT_READY_Y, "READY", COL_OK, COL_BG, 1);
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

    /* RescuePulse-style header band. Device name left-aligned, rate stamp
     * right-aligned in yellow, so the band reads as two anchored ends.
     * Name colour is COL_UPPER, the same electric magenta the boot splash
     * uses for SYNTHWAVE, so the two screens match. */
    ST7735_FillRect(0, 0, ST7735_WIDTH, HEADER_H, COL_PANEL);
    ST7735_DrawString(HEADER_PAD, HEADER_TEXT_Y, "SYNTHWAVE", COL_UPPER, COL_PANEL);

    build_rate_stamp();
    ST7735_DrawString((int16_t)(ST7735_WIDTH - HEADER_PAD -
                                ((int16_t)strlen(rate_stamp) * 8)),
                      HEADER_TEXT_Y, rate_stamp, COL_YELLOW, COL_PANEL);
    ST7735_DrawHLine(0, SEP_Y, ST7735_WIDTH, COL_SEP);

    /* Spectrum panel: dark frame, black interior, baseline. */
    draw_panel_frame();

    /* Region label strip, below the panel divider and clear of the panel's
     * 3 unreachable bottom rows. */
    ST7735_DrawHLine(PANEL_X, LABEL_SEP_Y, (int16_t)(PANEL_RIGHT - PANEL_X + 1), COL_SEP);
    ST7735_DrawString(LABEL_LO_X,  LABEL_Y, "LOW", COL_TEXT_DIM, COL_BG);
    ST7735_DrawString(LABEL_MID_X, LABEL_Y, "MID", COL_TEXT_DIM, COL_BG);
    ST7735_DrawString(LABEL_HI_X,  LABEL_Y, "HI",  COL_TEXT_DIM, COL_BG);

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

        /* Pick this column's source band height. merge_lo/merge_hi are the
         * identity today (1:1 passthrough), so this is just bands[i]; the
         * two-sided form is kept so band pairing stays a table edit. */
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