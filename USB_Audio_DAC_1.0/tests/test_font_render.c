/* tests/test_font_render.c
 *
 * Host test for the ST7735 text renderer's pixel model (must mirror
 * ST7735_DrawStringScaled in Core/Src/st7735.c):
 *
 *   - font8x8 is ROW-major: byte[0] is the top row, bit 7 is the left
 *     pixel.
 *   - Emission is row OUTER / column INNER (ST7735 fills a RAM window
 *     with X incrementing fastest). The old column-outer order
 *     transposed every glyph -- that regression is what this test pins.
 *
 * Build: gcc -I../Core/Src test_font_render.c -o test_font_render
 */

#include "font8x8.h"
#include <stdio.h>
#include <string.h>

#define W 128
#define H 128
#define FG 0x073E
#define BG 0x0000

static uint16_t grid[H * W];

/* Replicates the renderer's glyph -> pixels mapping into `grid`. */
static void render_char(int16_t x, int16_t y, char c, int16_t scale)
{
    unsigned char uc = (unsigned char)c;
    const uint8_t *g;
    int16_t col, row;

    if (uc < 32U || uc > 126U) {
        uc = '?';
    }
    g = font8x8[uc - 32U];

    for (row = 0; row < (int16_t)(8 * scale); row++) {
        uint8_t bits = g[row / scale];
        for (col = 0; col < (int16_t)(8 * scale); col++) {
            uint16_t color = ((bits >> (7 - col / scale)) & 0x01U) ? FG : BG;
            grid[(y + row) * W + (x + col)] = color;
        }
    }
}

static void clear_grid(void)
{
    memset(grid, 0, sizeof(grid));
}

static int expect_px(int line, int16_t x, int16_t y, uint16_t want)
{
    uint16_t got = grid[y * W + x];
    if (got != want) {
        fprintf(stderr,
                "  line %d: pixel(%d,%d) = 0x%04X, expected 0x%04X\n",
                line, x, y, got, want);
        return 1;
    }
    return 0;
}

/* Guards the font table layout itself. */
static int test_font_bytes(void)
{
    int fails = 0;

    if (font8x8['A' - 32][0] != 0x18    || font8x8['A' - 32][4] != 0x7E) fails++;
    if (font8x8['0' - 32][0] != 0x3C    || font8x8['0' - 32][6] != 0x3C) fails++;
    if (font8x8['5' - 32][0] != 0x7E    || font8x8['5' - 32][2] != 0x7C) fails++;
    if (font8x8[' ' - 32][7] != 0x00)                                          fails++;
    return fails;
}

static int test_glyph_a(void)
{
    int fails = 0;

    clear_grid();
    render_char(0, 0, 'A', 1);

    /* 'A' rows: 0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00 (bit7=left) */
    fails += expect_px(__LINE__, 0, 0, BG);   /* top-left open        */
    fails += expect_px(__LINE__, 4, 0, FG);   /* row0 right-side dome */
    fails += expect_px(__LINE__, 1, 1, BG);   /* row1 starts at col2  */
    fails += expect_px(__LINE__, 2, 1, FG);
    fails += expect_px(__LINE__, 1, 4, FG);   /* row4 crossbar spans 1..6 */
    fails += expect_px(__LINE__, 6, 4, FG);
    fails += expect_px(__LINE__, 7, 6, BG);   /* bottom rows empty    */
    fails += expect_px(__LINE__, 0, 3, BG);   /* col0 open all rows   */
    return fails;
}

static int test_glyph_bang(void)
{
    int fails = 0;

    clear_grid();
    render_char(0, 0, '!', 1);
    fails += expect_px(__LINE__, 4, 0, FG);
    fails += expect_px(__LINE__, 0, 4, BG);   /* transpose regression guard */
    fails += expect_px(__LINE__, 4, 6, FG);
    fails += expect_px(__LINE__, 4, 7, BG);
    return fails;
}

static int test_glyph_bang_2x(void)
{
    int fails = 0;

    clear_grid();
    render_char(0, 0, '!', 2);
    /* '!' row0 = 0x18 -> source cols 3,4 set; each becomes a 2x2 block */
    fails += expect_px(__LINE__, 0, 0, BG);   /* u=0 (bit7) empty       */
    fails += expect_px(__LINE__, 4, 0, BG);   /* u=2 (bit5) empty       */
    fails += expect_px(__LINE__, 6, 0, FG);   /* u=3 (bit4) set         */
    fails += expect_px(__LINE__, 9, 0, FG);   /* u=4 (bit3) set         */
    fails += expect_px(__LINE__, 6, 1, FG);   /* vertical block expansion */
    fails += expect_px(__LINE__, 6, 3, FG);   /* row3 (0x18) same cols  */
    fails += expect_px(__LINE__, 6, 8, FG);   /* row4 (0x18), draw row 8 */
    fails += expect_px(__LINE__, 6, 15, BG);  /* bottom row (0x00)      */
    return fails;
}

int main(void)
{
    int fails = 0;

    fails += test_font_bytes();
    fails += test_glyph_a();
    fails += test_glyph_bang();
    fails += test_glyph_bang_2x();

    if (fails != 0) {
        printf("test_font_render: FAILED (%d)\n", fails);
        return 1;
    }
    printf("test_font_render: OK\n");
    return 0;
}