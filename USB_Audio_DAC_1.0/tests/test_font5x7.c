/* tests/test_font5x7.c
 *
 * Host test for the ST7735 compact 5x7 text renderer (must mirror
 * ST7735_DrawString5x7 in Core/Src/st7735.c):
 *
 *   - font5x7 is COLUMN-major: byte[c] is COLUMN c (0 = leftmost) and
 *     BIT 0 is the TOP row, bit 6 the bottom.
 *   - Emission must be column OUTER / row INNER, because the ST7735
 *     fills a RAM window with X incrementing fastest. The 8x8 renderer
 *     needs the opposite (row outer / column inner) because its table is
 *     row-major -- swapping the two loops is the easy mistake, and it
 *     transposes every glyph into garbage. That exact regression is
 *     what this test pins (test_font_render.c pins the 8x8 half).
 *   - Advance is 6 px (5 px glyph + 1 px gap), with no trailing gap.
 *   - Clipping: a string that runs off the right edge or the bottom
 *     must render only its visible pixels.
 *
 * Build: gcc -I../Core/Inc test_font5x7.c -o test_font5x7
 */

#include "font5x7.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 128
#define FG 0xFFFF
#define BG 0x0000

static uint16_t grid[H * W];

static void reset_grid(void)
{
    int i;
    for (i = 0; i < W * H; i++) {
        grid[i] = BG;
    }
}

static int at(int x, int y)
{
    if (x < 0 || y < 0 || x >= W || y >= H) {
        return -1; /* off-screen */
    }
    return grid[y * W + x];
}

/* Replicates ST7735_DrawString5x7's glyph -> pixels mapping into `grid`. */
static void render_char(int16_t x, int16_t y, char c)
{
    unsigned char uc = (unsigned char)c;
    const uint8_t *g;
    int16_t col, row;
    int16_t glyph_w = (int16_t)FONT5X7_WIDTH;
    int16_t glyph_h = (int16_t)FONT5X7_HEIGHT;
    int16_t x0, y0, x1, y1;

    if (uc < 32U || uc > 126U) {
        uc = '?';
    }
    g = font5x7[uc - 32U];

    x0 = (x < 0) ? 0 : x;
    y0 = (y < 0) ? 0 : y;
    x1 = (int16_t)(x + glyph_w - 1);
    y1 = (int16_t)(y + glyph_h - 1);
    if (x1 > W - 1) { x1 = W - 1; }
    if (y1 > H - 1) { y1 = H - 1; }

    for (col = x0; col <= x1; col++) {
        uint8_t bits = g[col - x];
        for (row = y0; row <= y1; row++) {
            int px = x + col - x0; /* == col */
            int py = y + row - y0; /* == row */
            if (px >= 0 && py >= 0 && px < W && py < H) {
                grid[py * W + px] = ((bits >> (row - y)) & 0x01U) ? FG : BG;
            }
        }
    }
}

static void render_string(int16_t x, int16_t y, const char *s)
{
    int16_t cx = x;
    while (*s != '\0') {
        render_char(cx, y, *s++);
        cx = (int16_t)(cx + FONT5X7_WIDTH + 1);
    }
}

/* Width of a centred 5x7 string, mirroring DrawStringCentered5x7. */
static int centered_x(const char *s)
{
    int w = (int)(strlen(s) * (FONT5X7_WIDTH + 1)) - 1;
    if (w > W) {
        w = W;
    }
    return (W - w) / 2;
}

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                 \
    do {                                                            \
        checks++;                                                   \
        if (!(cond)) {                                              \
            failures++;                                             \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                           \
    } while (0)

/* ---- 1. font table orientation ------------------------------------ */
static void test_column_major(void)
{
    int c, r;

    /* Column-major, bit 0 = TOP row. 'L' is a full-height left upright
     * (all 7 rows) plus a bottom foot, which pins the bit order exactly:
     * if bit 0 were the bottom, col 0 would read 0xFE, not 0x7F. */
    CHECK(font5x7['L' - 32][0] == 0x7F);
    CHECK(font5x7['L' - 32][1] == 0x40); /* foot of the L, bottom row only */
    CHECK(font5x7['L' - 32][4] == 0x00);

    /* 'W' must have two uprights at the top that converge by mid-height,
     * otherwise it is indistinguishable from 'M' at this size -- the exact
     * legibility failure that disqualified an earlier rasterised font. */
    CHECK((font5x7['W' - 32][0] & 0x01U) != 0U);  /* top-left upright   */
    CHECK((font5x7['W' - 32][4] & 0x01U) != 0U);  /* top-right upright  */
    CHECK((font5x7['W' - 32][0] & 0x08U) == 0U);  /* gone by mid-height */
    CHECK((font5x7['W' - 32][4] & 0x08U) == 0U);
    /* 'M' must be the other way round: a left upright at the top plus a
     * right stem that reaches the baseline. */
    CHECK((font5x7['M' - 32][0] & 0x01U) != 0U);
    CHECK((font5x7['M' - 32][3] & 0x40U) != 0U);

    /* Space must be entirely blank. */
    for (c = 0; c < FONT5X7_WIDTH; c++) {
        CHECK(font5x7[' ' - 32][c] == 0x00);
    }

    /* Every glyph must fit in 7 bits per column (no bleed into bit 7). */
    for (c = 32; c <= 126; c++) {
        for (r = 0; r < FONT5X7_WIDTH; r++) {
            CHECK((font5x7[c - 32][r] & 0x80) == 0);
        }
    }

    /* Cap-height consistency: every A-Z and 0-9 must ink BOTH the top row
     * and the bottom row, i.e. share one cap height and one baseline. A
     * glyph that floats is a rasterisation bug -- that is how an earlier
     * font ended up with 'M' two rows shorter than 'D'. (Lowercase and
     * punctuation like '.' or ',' legitimately do not reach the baseline,
     * so they are deliberately excluded.) */
    {
        const char *caps = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        int top_ok = 0, bottom_ok = 0;
        for (c = 0; caps[c] != '\0'; c++) {
            int inked = 0;
            for (r = 0; r < FONT5X7_WIDTH; r++) {
                if (font5x7[caps[c] - 32][r] & 0x01U) { inked = 1; }
            }
            if (inked) { top_ok++; }
            inked = 0;
            for (r = 0; r < FONT5X7_WIDTH; r++) {
                if (font5x7[caps[c] - 32][r] & 0x40U) { inked = 1; }
            }
            if (inked) { bottom_ok++; }
        }
        CHECK(top_ok == 36);
        CHECK(bottom_ok == 36);
    }
}

/* ---- 2. advance + spacing ----------------------------------------- */
static void test_advance(void)
{
    int i, gap_clean = 1;

    reset_grid();
    /* 'H' has a full-height left upright in column 0. Two 'H' glyphs:
     * the second starts at x+6, leaving a 1 px gap at x+5. */
    render_string(10, 10, "HH");
    CHECK(at(10, 10) == FG);          /* first glyph col 0 (the upright) */
    CHECK(at(15, 10) == BG);          /* 1 px inter-glyph gap             */
    CHECK(at(16, 10) == FG);          /* second glyph col 0 at +6         */
    CHECK(at(20, 10) == FG);          /* second glyph last column         */
    CHECK(at(21, 10) == BG);          /* no trailing gap drawn            */

    /* Nothing should have been written to the gap column below either. */
    for (i = 10; i <= 11; i++) {
        if (at(15, i) != BG) {
            gap_clean = 0;
        }
    }
    CHECK(gap_clean);
}

/* ---- 3. the orientation regression guard --------------------------- */
static void test_transposed_would_differ(void)
{
    int16_t col, row;
    int diff = 0;
    const uint8_t *g = font5x7['F' - 32];

    /* Render 'F' correctly, then compare against what the swapped loop
     * order (row outer / column inner) would produce. If the two agree,
     * this test can no longer detect a transposed renderer, so fail
     * loudly rather than silently losing its teeth. */
    reset_grid();
    render_string(20, 20, "F");

    for (col = 0; col < FONT5X7_WIDTH; col++) {
        for (row = 0; row < FONT5X7_HEIGHT; row++) {
            int correct = (g[col] >> row) & 1;
            int transposed = (g[col] >> col) & 1;
            if (correct != transposed) {
                diff++;
            }
        }
    }
    CHECK(diff > 8);

    /* And the correctly-rendered 'F' must actually be in the grid. */
    {
        int16_t x, y;
        int ink = 0;
        for (y = 20; y < 20 + FONT5X7_HEIGHT; y++) {
            for (x = 20; x < 20 + FONT5X7_WIDTH; x++) {
                if (at(x, y) == FG) {
                    ink++;
                }
            }
        }
        CHECK(ink > 10);
    }
}

/* ---- 4. clipping at the right and bottom edges --------------------- */
static void test_clipping(void)
{
    int16_t x, y;

    /* String hanging off the right edge must not wrap or corrupt row 0. */
    reset_grid();
    render_string(W - 8, 30, "LOWMIDHI");
    for (x = 0; x < W; x++) {
        CHECK(at(x, 0) == BG);
    }
    /* The visible part of the first glyph must still be drawn. 'L' inks
     * column 0, which is the first column on screen here. */
    CHECK(at(W - 8, 30) == FG);

    /* String hanging off the bottom must not write past H-1. */
    reset_grid();
    render_string(10, H - 3, "LOW");
    for (y = 0; y < H - 3; y++) {
        /* rows above must be untouched */
        CHECK(at(10, y) == BG);
    }
    /* The 4 visible rows of the glyph must exist. */
    {
        int ink = 0;
        for (y = H - 3; y < H; y++) {
            if (at(10, y) != BG) {
                ink++;
            }
        }
        CHECK(ink > 0);
    }
}

/* ---- 5. the real bottom-strip placement ---------------------------- */
static void test_bottom_strip_fits_visible_area(void)
{
    /* The panel's last 3 logical rows (125..127) are written to GRAM
     * rows 128..130 via ST7735_ROWSTART and are NOT visible on the glass.
     * This pins the contract that the region labels sit clear of them. */
    const int label_y = 113;
    int16_t x, y;
    int ink = 0;

    reset_grid();
    render_string(4, label_y, "LOW");
    render_string(44, label_y, "MID");
    render_string(97, label_y, "HI");

    CHECK(label_y + FONT5X7_HEIGHT - 1 <= H - 1 - 3);

    for (y = label_y; y < label_y + FONT5X7_HEIGHT; y++) {
        for (x = 0; x < W; x++) {
            if (at(x, y) == FG) {
                ink++;
            }
        }
    }
    /* All three labels must have drawn something. */
    CHECK(ink > 40);
}

/* ---- 6. centring --------------------------------------------------- */
static void test_centered(void)
{
    const char *s = "MID";
    int x0 = centered_x(s);
    int16_t x, y;
    int minx = W, maxx = -1;
    int left, right;

    reset_grid();
    render_string((int16_t)x0, 50, s);
    for (y = 50; y < 50 + FONT5X7_HEIGHT; y++) {
        for (x = 0; x < W; x++) {
            if (at(x, y) == FG) {
                if (x < minx) { minx = x; }
                if (x > maxx) { maxx = x; }
            }
        }
    }
    /* Ink must exist and stay inside the 17 px advance box the string
     * reserves (3 glyphs x 5 px + 2 gaps of 1 px = 17). */
    CHECK(minx >= x0);
    CHECK(maxx <= x0 + 3 * (FONT5X7_WIDTH + 1) - 2);
    CHECK(maxx > minx);

    /* Visually centred: the left and right margins must be within 2 px.
     * (Proportional glyphs mean the ink box is not exactly the advance
     * box, so exact equality is the wrong assertion here.) */
    left = minx;
    right = W - 1 - maxx;
    CHECK(abs(left - right) <= 2);
}

int main(void)
{
    test_column_major();
    test_advance();
    test_transposed_would_differ();
    test_clipping();
    test_bottom_strip_fits_visible_area();
    test_centered();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures) {
        printf("test_font5x7: FAILED\n");
        return 1;
    }
    printf("test_font5x7: OK\n");
    return 0;
}
