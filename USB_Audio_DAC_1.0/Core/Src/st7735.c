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
#include <stddef.h>

/* Panel glass offset: ST7735S 128x128 RAM window sits at col=2, row=3.
 * Verify on hardware; adjust these two if the image is shifted. */
#define ST7735_COLSTART  2
#define ST7735_ROWSTART  3

/* ST7735S command set (subset used by this driver). */
#define ST7735_SWRESET  0x01U  /* Software reset                   */
#define ST7735_SLPOUT   0x11U  /* Sleep out                        */
#define ST7735_COLMOD   0x3AU  /* Interface pixel format           */
#define ST7735_MADCTL   0x36U  /* Memory data access control       */
#define ST7735_INVON    0x21U  /* Display inversion on             */
#define ST7735_NORON    0x13U  /* Normal display mode on           */
#define ST7735_DISPON   0x29U  /* Display on                       */
#define ST7735_CASET    0x2AU  /* Column address set               */
#define ST7735_RASET    0x2BU  /* Row address set                  */
#define ST7735_RAMWR    0x2CU  /* Memory write                     */

/* SPI1 handle — owned by this file (no CubeMX hspi1). */
static SPI_HandleTypeDef hspi1;

/* Stack chunk for FillRect pixel streaming: 64 bytes = 32 RGB565 pixels. */
#define ST7735_CHUNK_BYTES  64U

/* ----------------------------------------------------------------------------
 * Public-domain 6x8 font, ASCII 32..126 (95 glyphs).
 * Column-major, 6 bytes per glyph; standard table used by Adafruit /
 * Bodmer-style ST7735 drivers. Bit 0 = top pixel of the column.
 * -------------------------------------------------------------------------- */
static const uint8_t font6x8[95][6] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 32 ' ' */
    {0x00, 0x00, 0x2F, 0x00, 0x00, 0x00}, /* 33 '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00, 0x00}, /* 34 '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14, 0x00}, /* 35 '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12, 0x00}, /* 36 '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62, 0x00}, /* 37 '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50, 0x00}, /* 38 '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00, 0x00}, /* 39 ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00, 0x00}, /* 40 '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00, 0x00}, /* 41 ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14, 0x00}, /* 42 '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08, 0x00}, /* 43 '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00, 0x00}, /* 44 ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x00}, /* 45 '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00, 0x00}, /* 46 '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02, 0x00}, /* 47 '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00}, /* 48 '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00, 0x00}, /* 49 '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46, 0x00}, /* 50 '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31, 0x00}, /* 51 '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10, 0x00}, /* 52 '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39, 0x00}, /* 53 '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30, 0x00}, /* 54 '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03, 0x00}, /* 55 '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36, 0x00}, /* 56 '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E, 0x00}, /* 57 '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00, 0x00}, /* 58 ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00, 0x00}, /* 59 ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00, 0x00}, /* 60 '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14, 0x00}, /* 61 '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08, 0x00}, /* 62 '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06, 0x00}, /* 63 '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E, 0x00}, /* 64 '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00}, /* 65 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36, 0x00}, /* 66 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22, 0x00}, /* 67 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C, 0x00}, /* 68 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41, 0x00}, /* 69 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01, 0x00}, /* 70 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A, 0x00}, /* 71 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00}, /* 72 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00, 0x00}, /* 73 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01, 0x00}, /* 74 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41, 0x00}, /* 75 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40, 0x00}, /* 76 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F, 0x00}, /* 77 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00}, /* 78 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00}, /* 79 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06, 0x00}, /* 80 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E, 0x00}, /* 81 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46, 0x00}, /* 82 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31, 0x00}, /* 83 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01, 0x00}, /* 84 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F, 0x00}, /* 85 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00}, /* 86 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F, 0x00}, /* 87 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63, 0x00}, /* 88 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07, 0x00}, /* 89 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43, 0x00}, /* 90 'Z' */
    {0x00, 0x7F, 0x41, 0x41, 0x00, 0x00}, /* 91 '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20, 0x00}, /* 92 '\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00, 0x00}, /* 93 ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04, 0x00}, /* 94 '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40, 0x00}, /* 95 '_' */
    {0x00, 0x01, 0x02, 0x04, 0x00, 0x00}, /* 96 '`' */
    {0x20, 0x54, 0x54, 0x54, 0x78, 0x00}, /* 97 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38, 0x00}, /* 98 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20, 0x00}, /* 99 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F, 0x00}, /* 100 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18, 0x00}, /* 101 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02, 0x00}, /* 102 'f' */
    {0x0C, 0x52, 0x52, 0x52, 0x3E, 0x00}, /* 103 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78, 0x00}, /* 104 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00, 0x00}, /* 105 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00, 0x00}, /* 106 'j' */
    {0x7F, 0x10, 0x28, 0x44, 0x00, 0x00}, /* 107 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00, 0x00}, /* 108 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78, 0x00}, /* 109 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78, 0x00}, /* 110 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38, 0x00}, /* 111 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08, 0x00}, /* 112 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C, 0x00}, /* 113 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08, 0x00}, /* 114 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20, 0x00}, /* 115 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20, 0x00}, /* 116 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C, 0x00}, /* 117 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C, 0x00}, /* 118 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C, 0x00}, /* 119 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44, 0x00}, /* 120 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C, 0x00}, /* 121 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44, 0x00}, /* 122 'z' */
    {0x00, 0x08, 0x36, 0x41, 0x00, 0x00}, /* 123 '{' */
    {0x00, 0x00, 0x7F, 0x00, 0x00, 0x00}, /* 124 '|' */
    {0x00, 0x41, 0x36, 0x08, 0x00, 0x00}, /* 125 '}' */
    {0x08, 0x08, 0x2A, 0x1C, 0x08, 0x00}, /* 126 '~' */
};

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
        uint8_t madctl = 0x00U;
        ST7735_WriteData(&madctl, 1U);
    }

    ST7735_WriteCommand(ST7735_INVON);  /* ST7735S 1.44" typically needs inversion */
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

void ST7735_DrawString(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    int16_t cx = x;
    uint8_t buf[6 * 8 * 2]; /* one glyph: 48 pixels, BE-packed */

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
        glyph = font6x8[c - 32U];

        /* Visible sub-rectangle of this 6x8 glyph (clip to screen). */
        x0 = (cx < 0) ? 0 : cx;
        y0 = (y  < 0) ? 0 : y;
        x1 = (int16_t)(cx + 5);
        y1 = (int16_t)(y + 7);
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
        cx = (int16_t)(cx + 6);
    }
}
