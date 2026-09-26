/* Core/Src/st7735.c
 *
 * ST7735S 128x128 SPI TFT driver (SPI1).
 *
 * This module owns SPI1 entirely: CubeMX does not generate MX_SPI1_Init
 * (no hspi1 in the .ioc), so GPIO + SPI peripheral enable + HAL_SPI_Init
 * all live here. Safe against .ioc regeneration as long as the pins stay
 * PA5/PA7 (SPI1 AF5), PA0 (DC), PA1 (RST), PB0 (CS).
 *
 * Wiring (Black Pill STM32F411CEU6):
 *   PA5 = SPI1_SCK   (AF5)
 *   PA7 = SPI1_MOSI  (AF5)
 *   PA0 = D/C        (GPIO out)
 *   PA1 = RST        (GPIO out)
 *   PB0 = CS         (GPIO out)
 *   LED/backlight is tied to 3V3 — no pin.
 *
 * Clock: SYSCLK 48 MHz -> APB2 48 MHz (DIV1) -> SPI1 prescaler /4
 * = 12 MHz SCK (ST7735S max 18 MHz).
 *
 * RGB565 on the wire is big-endian (high byte first). Pack every
 * pixel as [color >> 8, color & 0xFF] — never pre-swap the value
 * (a double-swap would put the low byte first).
 */

#include "st7735.h"
#include "font8x8.h"
#include "font5x7.h"
#include <stddef.h>
#include <string.h>

/* Panel glass offset: ST7735S 128x128 RAM window sits at col=2, row=3.
 * Verify on hardware; adjust these two if the image is shifted. */
#define ST7735_COLSTART  2
#define ST7735_ROWSTART  3

/* PANEL CONFIGURATION (per-module — flip these if this unit differs):
 *
 *  - ST7735_USE_INVERSION: most 1.44" 128x128 ST7735S modules are
 *    "normally black" glass and must NOT invert (default 0). Forcing
 *    INVON on such a panel shows inverted video: black looks grainy
 *    gray-white, magenta reads green, cyan reads brown. If instead your
 *    panel is "normally white", set this to 1 (classic ST7735 modules).
 *  - ST7735_USE_BGR: set 1 if red and blue look swapped on screen.
 *    This 1.44" 128x128 module is verified BGR (same panel family as the
 *    RescuePulse ST7735S reference build: invert ON = false, element order
 *    = BGR). Keep at 1 unless a hardware photo shows swapped colors.
 */
#define ST7735_USE_INVERSION  0U
#define ST7735_USE_BGR        1U

/* ST7735S command set (subset used by this driver). */
#define ST7735_SWRESET  0x01U  /* Software reset                   */
#define ST7735_SLPOUT   0x11U  /* Sleep out                        */
#define ST7735_COLMOD   0x3AU  /* Interface pixel format           */
#define ST7735_MADCTL   0x36U  /* Memory data access control       */
#define ST7735_INVON    0x21U  /* Display inversion on             */
#define ST7735_INVOFF   0x20U  /* Display inversion off            */
#define ST7735_NORON    0x13U  /* Normal display mode on           */
#define ST7735_DISPON   0x29U  /* Display on                       */
#define ST7735_CASET    0x2AU  /* Column address set               */
#define ST7735_RASET    0x2BU  /* Row address set                  */
#define ST7735_RAMWR    0x2CU  /* Memory write                     */

/* SPI1 handle — owned by this file (no CubeMX hspi1). */
static SPI_HandleTypeDef hspi1;

/* Stack chunk for FillRect pixel streaming: 64 bytes = 32 RGB565 pixels. */
#define ST7735_CHUNK_BYTES  64U

/* RGB565 is transmitted high-byte-first on the wire. STM32 is
 * little-endian, so pack explicitly: buf[0] = color >> 8 (high),
 * buf[1] = color & 0xFF (low). Never pre-swap the value. */
static void pack_be(uint16_t color, uint8_t *hi_lo)
{
    hi_lo[0] = (uint8_t)(color >> 8);
    hi_lo[1] = (uint8_t)(color & 0xFFU);
}

static void ST7735_WriteCommand(uint8_t cmd)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);   /* CS low  */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_RESET);   /* DC low  */
    HAL_SPI_Transmit(&hspi1, &cmd, 1U, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);     /* CS high */
}

static void ST7735_WriteData(const uint8_t *data, uint16_t len)
{
    if ((data == NULL) || (len == 0U))
    {
        return;
    }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);   /* CS low  */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_SET);     /* DC high */
    HAL_SPI_Transmit(&hspi1, (uint8_t *)data, len, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);     /* CS high */
}

/* PA0=DC, PA1=RST, PB0=CS as push-pull outs; PA5/PA7 as SPI1 AF5. */
static void ST7735_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Control pins: DC (PA0), RST (PA1), CS (PB0). */
    gpio.Pin   = GPIO_PIN_0 | GPIO_PIN_1;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_0;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* SPI1 pins: SCK (PA5), MOSI (PA7) — alternate function SPI1. */
    gpio.Pin       = GPIO_PIN_5 | GPIO_PIN_7;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* Idle CS high, RST high (active-low reset released). */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
}

/* SPI1 master, TX-only intent (MISO unused), 8-bit, mode 0, /4 = 12 MHz. */
static void MX_SPI1_Init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE();

    hspi1.Instance               = SPI1;
    hspi1.Init.Mode              = SPI_MODE_MASTER;
    hspi1.Init.Direction         = SPI_DIRECTION_2LINES; /* TX path only used */
    hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;     /* CPOL = 0 */
    hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;      /* CPHA = 0 */
    hspi1.Init.NSS               = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4; /* 48 MHz / 4 = 12 MHz */
    hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    HAL_SPI_Init(&hspi1);
}

/* Set the RAM address window (inclusive), applying panel glass offsets. */
static void SetAddressWindow(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    uint8_t ca[4];
    uint8_t ra[4];
    uint16_t xs = (uint16_t)x0 + ST7735_COLSTART;
    uint16_t xe = (uint16_t)x1 + ST7735_COLSTART;
    uint16_t ys = (uint16_t)y0 + ST7735_ROWSTART;
    uint16_t ye = (uint16_t)y1 + ST7735_ROWSTART;

    ca[0] = (uint8_t)(xs >> 8);
    ca[1] = (uint8_t)(xs & 0xFFU);
    ca[2] = (uint8_t)(xe >> 8);
    ca[3] = (uint8_t)(xe & 0xFFU);

    ra[0] = (uint8_t)(ys >> 8);
    ra[1] = (uint8_t)(ys & 0xFFU);
    ra[2] = (uint8_t)(ye >> 8);
    ra[3] = (uint8_t)(ye & 0xFFU);

    ST7735_WriteCommand(ST7735_CASET);
    ST7735_WriteData(ca, 4U);
    ST7735_WriteCommand(ST7735_RASET);
    ST7735_WriteData(ra, 4U);
    ST7735_WriteCommand(ST7735_RAMWR);
}

void ST7735_Init(void)
{
    ST7735_GPIO_Init();
    MX_SPI1_Init();

    /* Hardware reset: RST low 20 ms, high, wait 50 ms. */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET);
    HAL_Delay(20U);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
    HAL_Delay(50U);

    ST7735_WriteCommand(ST7735_SWRESET);
    HAL_Delay(150U); /* datasheet: wait 150 ms after SWRESET */

    ST7735_WriteCommand(ST7735_SLPOUT);
    HAL_Delay(120U); /* datasheet: wait 120 ms after SLPOUT */

    ST7735_WriteCommand(ST7735_COLMOD);
    {
        uint8_t colmod = 0x05U; /* 16-bit RGB565 */
        ST7735_WriteData(&colmod, 1U);
    }

    ST7735_WriteCommand(ST7735_MADCTL);
    {
        uint8_t madctl = (ST7735_USE_BGR != 0U) ? 0x08U : 0x00U; /* BGR flag */
        ST7735_WriteData(&madctl, 1U);
    }

    /* Inversion polarity is panel-dependent (see ST7735_USE_INVERSION). */
    if (ST7735_USE_INVERSION != 0U)
    {
        ST7735_WriteCommand(ST7735_INVON);
    }
    else
    {
        ST7735_WriteCommand(ST7735_INVOFF);
    }
    ST7735_WriteCommand(ST7735_NORON);
    HAL_Delay(10U);  /* normal mode settle */
    ST7735_WriteCommand(ST7735_DISPON);
    HAL_Delay(100U); /* display on settle */

    ST7735_FillScreen(0x0000U); /* black boot screen */
}

void ST7735_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    uint8_t buf[ST7735_CHUNK_BYTES];
    uint8_t px[2];
    uint32_t total;
    uint32_t i;

    if ((w <= 0) || (h <= 0))
    {
        return;
    }

    SetAddressWindow(x, y, (int16_t)(x + w - 1), (int16_t)(y + h - 1));

    /* Pre-fill chunk with big-endian color (32 pixels per 64-byte push). */
    pack_be(color, px);
    for (i = 0U; i < (ST7735_CHUNK_BYTES / 2U); i++)
    {
        buf[2U * i]      = px[0];
        buf[2U * i + 1U] = px[1];
    }

    total = (uint32_t)w * (uint32_t)h;
    while (total > 0U)
    {
        uint32_t pixels = total;
        uint16_t bytes;

        if (pixels > (ST7735_CHUNK_BYTES / 2U))
        {
            pixels = ST7735_CHUNK_BYTES / 2U;
        }
        bytes = (uint16_t)(pixels * 2U);
        ST7735_WriteData(buf, bytes);
        total -= pixels;
    }
}

void ST7735_FillScreen(uint16_t color)
{
    ST7735_FillRect(0, 0, ST7735_WIDTH, ST7735_HEIGHT, color);
}

void ST7735_DrawPixel(int16_t x, int16_t y, uint16_t color)
{
    uint8_t px[2];

    if ((x < 0) || (y < 0) || (x >= ST7735_WIDTH) || (y >= ST7735_HEIGHT))
    {
        return;
    }

    SetAddressWindow(x, y, x, y);
    pack_be(color, px);
    ST7735_WriteData(px, 2U);
}

void ST7735_DrawHLine(int16_t x, int16_t y, int16_t w, uint16_t color)
{
    ST7735_FillRect(x, y, w, 1, color);
}

void ST7735_DrawVLine(int16_t x, int16_t y, int16_t h, uint16_t color)
{
    ST7735_FillRect(x, y, 1, h, color);
}

static void ST7735_DrawStringScaled(int16_t x, int16_t y, const char *str,
                                    uint16_t fg, uint16_t bg, int16_t scale);

void ST7735_DrawString(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    ST7735_DrawStringScaled(x, y, str, fg, bg, 1);
}

void ST7735_DrawString2x(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    ST7735_DrawStringScaled(x, y, str, fg, bg, 2);
}

/* Compact 5x7 text renderer over the COLUMN-major font in font5x7.h.
 *
 * The loop order here is the mirror image of ST7735_DrawStringScaled and
 * that is deliberate, not a typo:
 *   - font8x8 is ROW-major (one byte per row)      -> emit row outer / col inner
 *   - font5x7 is COLUMN-major (one byte per column) -> emit col outer / row inner
 * The ST7735 always fills a RAM window with X incrementing fastest, so in
 * both cases the OUTER loop must be the one the font is indexed by.
 * Getting this backwards transposes every glyph (the exact regression the
 * host tests test_font_render / test_font5x7 exist to catch).
 *
 * Advance is 6 px: a 5 px glyph plus a 1 px gap. The trailing gap after
 * the last character is not drawn, so a centred string's width is
 * n*ST7735_ADVANCE5X7 - 1). */
void ST7735_DrawString5x7(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    int16_t cx = x;
    uint8_t buf[ST7735_GLYPH5X7_W * ST7735_GLYPH5X7_H * 2]; /* one glyph, BE-packed */
    int16_t glyph_w = (int16_t)ST7735_GLYPH5X7_W;
    int16_t glyph_h = (int16_t)ST7735_GLYPH5X7_H;

    if (str == NULL)
    {
        return;
    }

    while (*str != '\0')
    {
        unsigned char c = (unsigned char)*str++;
        const uint8_t *glyph;
        int16_t x0, y0, x1, y1;
        int16_t col, row;
        uint32_t n = 0U;

        if (c < 32U || c > 126U)
        {
            c = '?'; /* replace out-of-range with '?' */
        }
        glyph = font5x7[c - 32U];

        /* Visible sub-rectangle of this glyph (clip to screen). */
        x0 = (cx < 0) ? 0 : cx;
        y0 = (y  < 0) ? 0 : y;
        x1 = (int16_t)(cx + glyph_w - 1);
        y1 = (int16_t)(y + glyph_h - 1);
        if (x1 > ST7735_WIDTH - 1)  { x1 = ST7735_WIDTH - 1; }
        if (y1 > ST7735_HEIGHT - 1) { y1 = ST7735_HEIGHT - 1; }

        if ((x0 <= x1) && (y0 <= y1))
        {
            /* One address window per character; stream only visible pixels
             * so the byte count always matches the window size. */
            SetAddressWindow(x0, y0, x1, y1);

            for (col = x0; col <= x1; col++)
            {
                uint8_t bits = glyph[col - cx];
                for (row = y0; row <= y1; row++)
                {
                    uint16_t color = ((bits >> (row - y)) & 0x01U) ? fg : bg;
                    pack_be(color, &buf[n]);
                    n += 2U;
                }
            }
            ST7735_WriteData(buf, (uint16_t)n);
        }
        cx = (int16_t)(cx + ST7735_ADVANCE5X7);
    }
}

/* Centered horizontally using the 5x7 metrics (5 px glyph + 1 px gap,
 * no trailing gap). */
void ST7735_DrawStringCentered5x7(int16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    int16_t w;

    if (str == NULL)
    {
        return;
    }
    w = (int16_t)((strlen(str) * ST7735_ADVANCE5X7) - 1);
    if (w > ST7735_WIDTH)
    {
        w = ST7735_WIDTH;
    }
    ST7735_DrawString5x7((int16_t)((ST7735_WIDTH - w) / 2), y, str, fg, bg);
}

/* Centered horizontally (assuming font glyph width = 8 * scale). */
void ST7735_DrawStringCentered(int16_t y, const char *str, uint16_t fg,
                               uint16_t bg, int16_t scale)
{
    int16_t w;

    if (str == NULL)
    {
        return;
    }
    w = (int16_t)(strlen(str) * 8U * (uint16_t)scale);
    if (w > ST7735_WIDTH)
    {
        w = ST7735_WIDTH;
    }
    ST7735_DrawStringScaled((int16_t)((ST7735_WIDTH - w) / 2), y, str, fg, bg, scale);
}

/* Shared text renderer over the row-major 8x8 font in font8x8.h.
 * scale = 1 renders 8x8 glyphs; scale = 2 renders 16x16 blocks.
 *
 * The ST7735 fills a RAM write window with X (column) incrementing
 * fastest, i.e. row-major. Font bytes are also row-major with bit 7 =
 * leftmost pixel, so emission must be row outer / column inner; the
 * earlier column-outer order transposed every glyph into garbage. */
static void ST7735_DrawStringScaled(int16_t x, int16_t y, const char *str,
                                    uint16_t fg, uint16_t bg, int16_t scale)
{
    int16_t cx = x;
    uint8_t buf[16 * 16 * 2]; /* one 2x glyph: 16 cols x 16 rows, BE-packed */
    int16_t glyph_w = (int16_t)(8 * scale);
    int16_t glyph_h = (int16_t)(8 * scale);

    if (str == NULL)
    {
        return;
    }

    while (*str != '\0')
    {
        unsigned char c = (unsigned char)*str++;
        const uint8_t *glyph;
        int16_t x0, y0, x1, y1;
        int16_t col, row;
        uint32_t n = 0U;

        if (c < 32U || c > 126U)
        {
            c = '?'; /* replace out-of-range with '?' */
        }
        glyph = font8x8[c - 32U];

        /* Visible sub-rectangle of this scaled glyph (clip to screen). */
        x0 = (cx < 0) ? 0 : cx;
        y0 = (y  < 0) ? 0 : y;
        x1 = (int16_t)(cx + glyph_w - 1);
        y1 = (int16_t)(y + glyph_h - 1);
        if (x1 > ST7735_WIDTH - 1)  { x1 = ST7735_WIDTH - 1; }
        if (y1 > ST7735_HEIGHT - 1) { y1 = ST7735_HEIGHT - 1; }

        if ((x0 <= x1) && (y0 <= y1))
        {
            /* One address window per character; stream only visible pixels
             * so the byte count always matches the window size. */
            SetAddressWindow(x0, y0, x1, y1);

            for (row = y0; row <= y1; row++)
            {
                uint8_t bits = glyph[(row - y) / scale];
                for (col = x0; col <= x1; col++)
                {
                    uint16_t color = ((bits >> (7 - (col - cx) / scale)) & 0x01U) ? fg : bg;
                    pack_be(color, &buf[n]);
                    n += 2U;
                }
            }
            ST7735_WriteData(buf, (uint16_t)n);
        }
        cx = (int16_t)(cx + glyph_w);
    }
}
