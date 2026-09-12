# AGENTS.md — STM32F411 USB Audio DAC

## Project Overview

Embedded firmware for STM32F411CEU6 Black Pill USB Audio Class 1.0 device. Audio flows: PC → USB OTG FS → ring buffer → I2S2 + DMA → MAX98357A DAC → speaker.

**Status:** Phases 0–3 complete (toolchain, clocks, I2S+DMA, USB audio). Phases 4–7 planned (encoder volume, TFT visualizer, FreeRTOS, polish).

## Build & Flash

```bash
cd USB_Audio_DAC_1.0
make                    # Build (arm-none-eabi-gcc)
st-flash write build/USB_Audio_DAC_1.0.bin 0x08000000   # Flash
make clean              # Clean
```

**Toolchain:** `arm-none-eabi-gcc` v16.2.0, `st-flash` (ST-Link V2), `make`.

## Critical Debugging Gotchas

These caused silent failures (device enumerates but no audio):

1. **VBUS sensing** — Black Pill's PA9/VBUS line unreliable. Must set `vbus_sensing_enable = DISABLE` in `USB_DEVICE/Target/usbd_conf.c:342`.
2. **Sample rate mismatch** — `USBD_AUDIO_FREQ` in `usbd_conf.h` must equal the `.ioc` value (44100). Hardcoded 48000U causes ring over/underrun → silence.
3. **Missing `USBD_AUDIO_Sync` call** — ST library exports this but never calls it. Must invoke from I2S DMA callbacks in `Core/Src/stm32f4xx_it.c`. Order: (1) `HalfTransfer_CallBack_FS()`/`TransferComplete_CallBack_FS()`, then (2) `AudioI2S_RefillHalfA/B()`.

## I2S Pin Mapping (F411 specific)

| Function | Pin | AF |
|----------|-----|-----|
| I2S2 CK  | PB10 | AF5 |
| I2S2 WS  | PB12 | AF5 |
| I2S2 SD  | PB15 | AF5 |

**Not PB13** — common mistake from other STM32 families.

## Clock Configuration

- HSE = 25 MHz (external crystal)
- SYSCLK = 48 MHz (PLL: M=25, N=384, P=DIV8)
- USBCLK = 48 MHz (PLLQ=8)
- I2SCLK = 96 MHz (PLLI2S: M=25, N=192, R=2) — configured in `HAL_I2S_MspInit`, NOT `SystemClock_Config`
- Real I2S rate: 44.117 kHz (+0.04% error)

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

- `IMPLEMENTATION_PLAN.md` — Full 8-phase plan with task breakdowns
- `PROGRESS.md` — Phase-by-phase working log + debugging stories
- `BOM.md` — Bill of materials + pinout reference
- `STM32F411CEU6/` — Datasheet and reference manual PDFs

## Deferred Items

- Add `.ccmram` section to linker script (64 KB CCMRAM at 0x10000000, CPU-only)
- Bump stack from 0x800 to 0x1000 (needed for printf/FreeRTOS)
- Wire `AUDIO_VolumeCtl_FS` (currently no-op, Phase 4)
- 10-min playback stress test (Phase 3 acceptance)
