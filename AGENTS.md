# AGENTS.md — STM32F411 USB Audio DAC

## Project Overview

Embedded firmware for STM32F411CEU6 Black Pill USB Audio Class 1.0 device. Audio flows: PC → USB OTG FS → ring buffer → I2S2 + DMA → MAX98357A DAC → speaker.

**Status:** Phases 0–3, 3.5 complete (toolchain, clocks, I2S+DMA, USB audio, reliability/tests/watchdog). Phases 4–7 planned (encoder volume, TFT visualizer, FreeRTOS, polish).

## Build, Test & Flash

```bash
cd USB_Audio_DAC_1.0
make                    # Build debug (arm-none-eabi-gcc, -Og, -DDEBUG_NO_WATCHDOG)
make release            # Build release (-O2, watchdog enabled)
make test               # Host-simulated ring-buffer unit tests (host gcc, no ARM toolchain)
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

1. **VBUS sensing** — Black Pill's PA9/VBUS line unreliable. Must set `vbus_sensing_enable = DISABLE` in `USB_DEVICE/Target/usbd_conf.c:342`.
2. **Sample rate mismatch** — `USBD_AUDIO_FREQ` in `usbd_conf.h` must equal the `.ioc` value (44100). Hardcoded 48000U causes ring over/underrun → silence.
3. **Missing `USBD_AUDIO_Sync` call** — ST library exports this but never calls it. Must invoke from I2S DMA callbacks in `Core/Src/stm32f4xx_it.c`. Order: (1) `HalfTransfer_CallBack_FS()`/`TransferComplete_CallBack_FS()`, then (2) `AudioI2S_RefillHalfA/B()`.
4. **USB Audio OUT buffer spill pad & modulo wrap** — 44.1 kHz USB packets oscillate between 88 and 90 bytes (`AUDIO_OUT_PACKET_MAX = 90U`). The HAL linearly writes up to 90 bytes into `haudio->buffer[wr_ptr]`. Without `+ AUDIO_OUT_PACKET_MAX` padding at the end of `buffer[]`, writes near the logical end (`AUDIO_TOTAL_BUF_SIZE = 7040`) corrupt the struct's `rd_ptr`/`wr_ptr`/`control` fields, causing loud screeching and hard faults. Additionally, `wr_ptr` must wrap modulo (`wr_ptr -= AUDIO_TOTAL_BUF_SIZE`) instead of snapping to 0.
5. **No CCMRAM on STM32F411** — Unlike F405/F407, STM32F411 has no CCMRAM at `0x10000000`. Accessing this address immediately generates a BusFault/HardFault. All SRAM must reside in standard SRAM at `0x20000000`.

## I2S Pins & Clocks

Pin mappings (I2S2: **PB10=CK, PB12=WS, PB15=SD**, AF5 — not PB13) and the full clock tree (HSE 25 MHz → 48 MHz SYSCLK/USB, PLLI2S 96 MHz → real I2S rate 44.117 kHz) are single-sourced in **README.md → Wiring / Clock Configuration**.

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
| `USB_Audio_DAC_1.0/Core/Src/ring_buffer.c` | Lock-free SPSC ring buffer (1024 int16) |
| `USB_Audio_DAC_1.0/Core/Src/stm32f4xx_it.c` | Interrupt handlers + HAL callbacks |
| `USB_Audio_DAC_1.0/USB_DEVICE/App/usbd_audio_if.c` | USB audio → ring buffer bridge |
| `USB_Audio_DAC_1.0/USB_DEVICE/App/usbd_desc.c` | USB device/configuration descriptors |
| `USB_Audio_DAC_1.0/USB_DEVICE/Target/usbd_conf.c` | HAL PCD init, VBUS sensing disabled |
| `USB_Audio_DAC_1.0/USB_DEVICE/Target/usbd_conf.h` | `USBD_AUDIO_FREQ = 44100U` |

## Audio Pipeline

```
PC → USB OTG FS (88-byte packets, 1 ms) → usbd_audio_if.c → ring_buffer.c (SPSC, 23 ms) → audio_i2s.c → DMA1 Stream 4 → I2S2 → MAX98357A → speaker
```

## Testing

```bash
# Check USB enumeration
lsusb -v | grep -A 10 "Audio"

# Test with 1 kHz sine tone
speaker-test -D plughw:2,0 -c 1 -r 44100 -t sine -f 1000

# Play a WAV file
aplay -D plughw:2,0 your_audio.wav
```

## Reference Documents

- `README.md` — Overview, hardware/BOM/wiring, build & verify, status
- `WIRING.md` — Full MCU↔peripheral pin map + CubeMX config for upcoming phases
- `IMPLEMENTATION_PLAN.md` — Full 8-phase plan with task breakdowns
- `PROGRESS.md` — Phase-by-phase working log + changelog + debugging stories
- `STM32F411CEU6/` — Datasheet and reference manual PDFs

## Deferred Items

- Wire `AUDIO_VolumeCtl_FS` (currently no-op, Phase 4)
- 10-min playback stress test (Phase 3 acceptance)

## Done (removed from Deferred, 2026-09-17)

- `.ccmram` section added to linker script (64 KB CCMRAM at 0x10000000, CPU-only)
- Stack bumped 0x800 → 0x1000 (printf/FreeRTOS headroom)
