#ifndef __ST7735_H__
#define __ST7735_H__

#include "stm32f4xx_hal.h"
#include <stdint.h>

#define ST7735_WIDTH  128
#define ST7735_HEIGHT 128

/* Rows at the bottom of the logical framebuffer that are NOT visible on
 * the glass. SetAddressWindow adds ST7735_ROWSTART (3) to the row index
 * and MADCTL applies no rotation compensation, so logical rows
 * ST7735_HEIGHT-3 .. ST7735_HEIGHT-1 land in GRAM rows outside the panel's
 * active window and are silently discarded. Never draw text there. */
#define ST7735_HIDDEN_BOTTOM_ROWS 3
#define ST7735_USABLE_BOTTOM      (ST7735_HEIGHT - ST7735_HIDDEN_BOTTOM_ROWS)  /* 125 */

/* BOUNDS CONTRACT -- read before using the block/line primitives.
 *
 *   ST7735_DrawPixel  CLIPS. Out-of-range x/y are silently dropped.
 *
 *   ST7735_FillRect   DOES NOT CLIP. It only rejects w <= 0 or h <= 0,
 *                     then programs a GRAM window of (x, y) ..
 *                     (x + w - 1, y + h - 1) verbatim. A rect that runs
 *                     off an edge sets a window that wraps or overruns the
 *                     panel, and the result is garbage on the glass plus a
 *                     desynchronised address window for every later call.
 *                     The caller owns the bounds check.
 *   ST7735_DrawHLine / ST7735_DrawVLine are thin wrappers over FillRect
 *                     and inherit the same lack of clipping.
 *
 *   This asymmetry bit us before: the old region labels were drawn at
 *   y = 120, so an 8-row glyph reached y = 127 — inside ST7735_HEIGHT,
 *   so no check fired, yet those rows are physically invisible (see
 *   ST7735_HIDDEN_BOTTOM_ROWS above) and the strip rendered half-cut.
 *   The compile-time guards in visualizer.c exist to keep layout inside
 *   ST7735_USABLE_BOTTOM for the same reason.
 */

void ST7735_Init(void);
void ST7735_FillScreen(uint16_t color);
void ST7735_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void ST7735_DrawPixel(int16_t x, int16_t y, uint16_t color);
void ST7735_DrawString(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg);
void ST7735_DrawString2x(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg);
void ST7735_DrawStringCentered(int16_t y, const char *str, uint16_t fg, uint16_t bg, int16_t scale);
void ST7735_DrawHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
void ST7735_DrawVLine(int16_t x, int16_t y, int16_t h, uint16_t color);

#endif /* __ST7735_H__ */
