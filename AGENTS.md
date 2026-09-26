# AGENTS.md — STM32F411 USB Audio DAC

## Project Overview

Embedded firmware for STM32F411CEU6 Black Pill USB Audio Class 1.0 device. Audio flows: PC → USB OTG FS → ring buffer → I2S2 + DMA → MAX98357A DAC → speaker.

**Status:** Phases 0–3 and 3.5 complete (toolchain, clocks, I2S+DMA, USB audio, reliability/tests/watchdog), plus the Phase 5 TFT visualizer (ST7735S + 12-band FFT + boot splash) which is built, flashed and verified on the glass. Still to do: Phase 4 (encoder volume), Phase 6 (FreeRTOS), Phase 7 (polish).

> **Branches:** everything ships from `main`. The display work (Phase 5) was
> developed on `feat/boot-splash-horizon` and merged in PR #4; that branch is
> retained but is not needed to build. The 5x7 font that used to sit on `main`
> (`Core/Inc/font5x7.h`, `tests/test_font5x7.c`) is gone — there is exactly one
> font, `Core/Src/font8x8.h`.

## Build, Test & Flash

```bash
cd USB_Audio_DAC_1.0
make                    # Build debug (arm-none-eabi-gcc, -Og, -DDEBUG_NO_WATCHDOG)
make release            # Build release (-O2, watchdog enabled)
make test               # Host unit tests: ring buffer + audio_fft + font render (host gcc, no ARM toolchain)
make size               # Per-section memory usage
make flash              # st-flash write build/USB_Audio_DAC_1.0.bin 0x08000000
st-flash write build/USB_Audio_DAC_1.0.bin 0x08000000   # (same as make flash)
make clean              # Clean
```

**Toolchain:** `arm-none-eabi-gcc` v16.2.0, `st-flash` (ST-Link V2), `make`. Host tests need only `gcc`.

## Watchdog (IWDG)

- Enabled in `Core/Inc/stm32f4xx_hal_conf.h` (`HAL_IWDG_MODULE_ENABLED`).
- Started in `main.c` USER CODE 2 (~1 s window: LSI/64 = 500 Hz, reload 500); refreshed in the main loop (USER CODE 3).
- Resets the MCU if the **main loop** stalls. NB: it does not watch per-peripheral hangs (e.g., DMA) — audio silence with a live main loop still won't reset.

## Critical Debugging Gotchas

These caused silent failures (device enumerates but no audio) or crashes/screeching:

1. **VBUS sensing** — Black Pill's PA9/VBUS line unreliable. Must set `vbus_sensing_enable = DISABLE` in `USB_DEVICE/Target/usbd_conf.c:344`.
2. **Sample rate mismatch** — `USBD_AUDIO_FREQ` in `usbd_conf.h` must equal the `.ioc` value (**48000**), which must in turn equal the *real* I2S2 rate. The device now runs an exact 48 kHz (see gotcha 6); a hardcoded value that disagrees with either causes ring over/underrun → silence, or slow drain → periodic zero-gap "taps".
3. **Missing `USBD_AUDIO_Sync` call** — ST library exports this but never calls it. Must invoke from I2S DMA callbacks in `Core/Src/stm32f4xx_it.c`. Order: (1) `HalfTransfer_CallBack_FS()`/`TransferComplete_CallBack_FS()`, then (2) `AudioI2S_RefillHalfA/B()`.
4. **USB Audio OUT buffer spill pad & modulo wrap** — the HAL linearly writes up to `AUDIO_OUT_PACKET_MAX` bytes into `haudio->buffer[wr_ptr]`. Without `+ AUDIO_OUT_PACKET_MAX` padding at the end of `buffer[]`, writes near the logical end (`AUDIO_TOTAL_BUF_SIZE = 7680` at 48 kHz) corrupt the struct's `rd_ptr`/`wr_ptr`/`control` fields, causing loud screeching and hard faults. Additionally, `wr_ptr` must wrap modulo (`wr_ptr -= AUDIO_TOTAL_BUF_SIZE`) instead of snapping to 0. (At 44.1 kHz this was 88/90 bytes because of the 1-in-10 "long frame"; at 48 kHz every frame is exactly 96 bytes, so `AUDIO_OUT_PACKET_MAX = 96U` is exact, but the padding requirement is unchanged.)
5. **Exact 44.1 kHz is impossible on this board** — see README → Clock Configuration. The device is 48 kHz exact by design. Don't "fix" a rate problem by editing only `usbd_conf.h`; the real rate comes from `PLLI2SN/PLLI2SR` in `stm32f4xx_hal_msp.c` plus `I2S_AUDIOFREQ_48K` in `main.c`, and all three (plus the `.ioc`) must agree.
6. **No CCMRAM on STM32F411** — unlike F405/F407/F415/F417/F429, the F411 has **no** CCM data SRAM; `0x10000000` is unmapped and any access BusFaults/HardFaults immediately. Evidence: ST's own linker templates define a CCMRAM region for F405/407/415/417/427/429/437/439/469/479 but **not** for F401/410/411/412/413. All SRAM must live in standard SRAM at `0x20000000` (128 KB here). `STM32F411xx_FLASH.ld` still declares an empty `CCMRAM` region inherited from an F4x7 template — it is a trap, not a feature; never tag anything `__attribute__((section(".ccmram")))`.
7. **MAX98357A GAIN pin must be tied, not floating** — a floating GAIN produced audible ~200/300 Hz buzzy distortion that firmware changes could not remove. The `dbg_bypass_usb` generated-tone test in `audio_i2s.c` proved the I2S→amp path was clean, which pointed at the amp; tying **GAIN to GND** (12 dB) removed it. If that buzzy character returns, check GAIN before the firmware. See WIRING.md → GAIN.

## I2S Pins & Clocks

Pin mappings (I2S2: **PB10=CK, PB12=WS, PB15=SD**, AF5 — not PB13) and the full clock tree (HSE 25 MHz → 48 MHz SYSCLK/USB, PLLI2S 192 MHz → real I2S rate 48 000.000 Hz exact) are single-sourced in **README.md → Wiring / Clock Configuration**.

## CubeMX Code Generation

- Project uses CubeMX `.ioc` → Makefile toolchain
- **Always respect `USER CODE BEGIN/END` blocks** — code outside these is overwritten on regeneration
- To add new source files: edit `Makefile` C_SOURCES list, then regenerate if needed
- `.ioc` file: `USB_Audio_DAC_1.0/USB_Audio_DAC_1.0.ioc`

## Key Files

| File | Purpose |
|------|---------|
| `USB_Audio_DAC_1.0/Core/Src/main.c` | System init, clock config, I2S + USB init |
| `USB_Audio_DAC_1.0/Core/Src/audio_i2s.c` | I2S DMA consumer (ring → stereo frames) |
| `USB_Audio_DAC_1.0/Core/Src/ring_buffer.c` | Lock-free SPSC ring buffer (2048 int16 = 42.7 ms @ 48 kHz) |
| `USB_Audio_DAC_1.0/Core/Src/audio_fft.c` | 1024-pt FFT → 12 perceptual band heights |
| `USB_Audio_DAC_1.0/Core/Src/visualizer.c` | Boot splash + spectrum screen, panel/label layout |
| `USB_Audio_DAC_1.0/Core/Src/st7735.c` | ST7735S SPI driver (owns SPI1; no CubeMX `hspi1`) |
| `USB_Audio_DAC_1.0/Core/Src/font8x8.h` | The one and only font (8x8, row-major) |
| `USB_Audio_DAC_1.0/Core/Src/stm32f4xx_it.c` | Interrupt handlers + HAL callbacks |
| `USB_Audio_DAC_1.0/USB_DEVICE/App/usbd_audio_if.c` | USB audio → ring buffer bridge |
| `USB_Audio_DAC_1.0/USB_DEVICE/App/usbd_desc.c` | USB device/configuration descriptors |
| `USB_Audio_DAC_1.0/USB_DEVICE/Target/usbd_conf.c` | HAL PCD init, VBUS sensing disabled |
| `USB_Audio_DAC_1.0/USB_DEVICE/Target/usbd_conf.h` | `USBD_AUDIO_FREQ = 48000U` |

## Audio Pipeline

```
PC → USB OTG FS (96-byte packets, 1 ms) → usbd_audio_if.c → ring_buffer.c (SPSC, 2048 samples) → audio_i2s.c → DMA1 Stream 4 → I2S2 → MAX98357A → speaker
```

## Testing

```bash
# Check USB enumeration
lsusb -v | grep -A 10 "Audio"

# Test with 1 kHz sine tone
speaker-test -D plughw:2,0 -c 1 -r 48000 -t sine -f 1000

# Play a WAV file
aplay -D plughw:2,0 your_audio.wav
```

## Reference Documents

This repo keeps four docs, one job each. Do not add a fifth without a reason.

| Document | Job |
|----------|-----|
| `README.md` | Overview, hardware/BOM/wiring, build & verify, status, **clock tree** |
| `WIRING.md` | Full MCU↔peripheral pin map, amp config, CubeMX config |
| `DEBUGGING.md` | Every bug hit: symptom → cause → fix, plus what is still open |
| `AGENTS.md` | This file — gotchas, key files, deferred items |

Single-source rule: code comments *point at* these docs rather than restating
their numbers, so a value is stated in exactly one place.

## Deferred Items

- Wire `AUDIO_VolumeCtl_FS` (currently no-op, Phase 4)
- 10-min playback stress test (Phase 3 acceptance)
- `RingBuffer_Reset()` is called from the OTG_FS control path (NVIC prio 3) while the priority-0
  I2S DMA handler can preempt it, so its two stores can be torn. Benign (a click at stream
  start), but documented rather than fixed. See the hazard note in `ring_buffer.c`.
- Rescale `audio_fft.c` `band_start[]` to restore the pre-48 kHz Hz labels (+8.8 % cosmetic drift)

## Done

- Stack bumped 0x800 → 0x1000 (printf/FreeRTOS headroom)
- The `CCMRAM` region and `.ccmram` section inherited from an F4x7 template were
  **removed** from `STM32F411xx_FLASH.ld` (gotcha 5). The F411 has no CCM data
  SRAM, so that region was a booby trap: a stray `.ccmram` tag would have linked
  fine and HardFaulted at runtime. Removing it makes such a tag fail the *link*.
