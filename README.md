# STM32F411 USB Audio Player

![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![STM32](https://img.shields.io/badge/MCU-STM32F411CEU6-blue.svg)
![HAL](https://img.shields.io/badge/Driver-STM32_HAL-orange.svg)
![USB Audio](https://img.shields.io/badge/USB-Audio%20Class%201.0-purple.svg)
![I2S](https://img.shields.io/badge/Protocol-I2S%20%2B%20DMA-red.svg)

A self-built USB speaker built on the **STM32F411CEU6 Black Pill**. When connected to a PC via USB, it enumerates as a standard USB Audio Class 1.0 device — no drivers needed. Audio data flows from the PC over USB, is decoded to analog via I2S, and played through a **MAX98357A** DAC + Class D amplifier driving an **8 Ω** speaker.

A rotary encoder (Phase 4) will control volume, and an ST7735S TFT (Phase 5) will display a real-time audio visualizer.

<!-- ![Hardware Setup](photos/hardware_setup.jpg) -->
<!-- ![USB Audio Playing](photos/playing.jpg) -->

---

## Features

- **USB Audio Class 1.0** — Plug-and-play USB speaker on Linux, Windows, macOS
- **48 kHz / 16-bit / Mono** audio over USB isochronous endpoint (exact rate; see Clock Configuration)
- **I2S + DMA** output to MAX98357A DAC + 3 W Class D amplifier
- **Lock-free SPSC ring buffer** — 42.7 ms of audio headroom between USB and I2S
- **Rotary encoder volume control** via software gain + mute button (Phase 4)
- **ST7735S TFT visualizer** with real-time audio level (Phase 5)
- **FreeRTOS-based** multitasking architecture (Phase 6)
- **Low cost** — ~$8-10 in parts per unit (see Bill of Materials)

---

## Hardware

The parts list is in [Bill of Materials](#bill-of-materials) below. This section
covers how they connect.

> **On amplifiers:** the MAX98357A is the *complete* DAC + amp for the mono speaker — it needs no second stage. The PAM8403 from the early plan was **descoped**: its line-level input cannot be driven from the MAX98357A's speaker-level output (cascading two power amps only distorts), so it had no role in a mono-speaker build. The 8 Ω speaker is verified by DCR: 7 Ω on the multimeter = nominal 8 Ω.

### Wiring summary

- **Audio out (I2S2 → MAX98357A):** PB10=BCLK, PB12=WS/LRC, PB15=SDIN, GND, 3V3/5V=VIN
- **USB OTG FS:** PA11=D-, PA12=D+
- **Encoder (Phase 4, planned):** PB6=A, PB7=B (TIM4 encoder mode), PB8=button (EXTI, mute) — *not wired; no volume control exists yet*
- **TFT (Phase 5, working):** SPI1 — see pinout below

### ST7735S TFT Pinout (SPI)

| Display Pin | Function | STM32F411 Pin |
|-------------|----------|---------------|
| VCC / GND | Power | 3V3 / GND |
| CS | Chip select (active low) | PB0 |
| RESET | Hardware reset (active low) | PA1 |
| DC | Data/Command (0 = cmd, 1 = data) | PA0 |
| SDA (MOSI) | SPI data | PA7 (SPI1_MOSI) |
| SCL (SCK) | SPI clock | PA5 (SPI1_SCK) |
| LED | Backlight (anode) | PA2 (optional — can tie to 3V3) |

**Connections:** SPI1 (PA5, PA7) + control pins (PB0, PA0, PA1, PA2) + power (3V3, GND) = 8 wires. CS is on **PB0** (kept from an earlier plan that routed DAC1 to PA4).

---

## Bill of Materials

| Item | Qty | Role | Cost |
|------|-----|------|------|
| STM32F411CEU6 Black Pill | 1 | Main MCU — Cortex-M4, 100 MHz, 128 KB RAM, 512 KB flash | ~$4 |
| MAX98357A module | 1 | I2S DAC + Class D amp (3 W @ 4 Ω, ~1.7 W @ 8 Ω) | ~$3 |
| 8 Ω speaker (DCR ≈ 7 Ω) | 1 | Mono audio output | ~$2 |
| ST7735S 1.4" TFT | 1 | Visualizer display, 128×128 SPI | ~$4 |
| ST-Link V2 | 1 | Flash programmer / debugger | ~$5 |
| Rotary encoder (EC11) | 1 | Volume + mute (Phase 4) | ~$1 |
| Passives (R, C) | various | Decoupling, filtering, encoder pull-ups | ~$1 |
| USB-C cable, jumpers, breadboard | — | Hookup | ~$5 |
| **Total** | | | **~$8–10 per unit** |

Volume and mute are Phase 4, so the device plays audio without the encoder.

**Where to buy:** AliExpress (cheapest, slowest), Amazon (faster, pricier), or a
local electronics shop. No USB-serial adapter is listed because the firmware has
no UART — debug output goes over SWD/GDB.

---

## Audio Pipeline

```
PC (USB audio source)
    │
    │  USB Full-Speed (12 Mbps, 48 kHz 16-bit mono)
    ▼
STM32F411 Black Pill
    │
    ├── USB OTG FS ─────── receives 96-byte packets (48 samples) every 1 ms, exactly
    │       │
    │       ▼
    │   usbd_audio_if.c ── AUDIO_CMD_PLAY → RingBuffer_Write()
    │       │
    │       ▼
    │   ring_buffer.c ──── SPSC ring, 2048 int16 = 42.7 ms headroom
    │       │
    │       ▼
    │   audio_i2s.c ────── RefillHalfA/B: mono → L+R stereo duplication
    │       │
    │       ▼
    ├── I2S2 + DMA1 ────── circular DMA, ping-pong halves (9.2 ms each)
    │       │
    │       ▼
    │   MAX98357A ──────── I2S DAC + Class D amp
    │       │
    │       ▼
    └──▶ 8 Ω Speaker
```

---

## Build & Flash

### Prerequisites

- `arm-none-eabi-gcc` (tested with v16.2.0)
- `st-flash` (ST-Link V2 flash tool)
- `make`
- ST-Link V2 programmer connected to the Black Pill

### Build, test & flash

```bash
cd USB_Audio_DAC_1.0
make                    # Build (arm-none-eabi-gcc)
make test               # Host unit tests: ring buffer + FFT + font render (host gcc, no ARM toolchain)
make size               # Per-section memory usage
make flash              # Flash via st-flash
make clean              # Clean
```

Equivalent manual flash: `st-flash write build/USB_Audio_DAC_1.0.bin 0x08000000`

### Verify (Linux)

After flashing, connect the Black Pill to your PC via USB-C:

```bash
# Check USB enumeration
lsusb -v | grep -A 10 "Audio"

# Test with a 1 kHz sine tone
speaker-test -D plughw:2,0 -c 1 -r 48000 -t sine -f 1000

# Play a WAV file
aplay -D plughw:2,0 your_audio.wav
```

---

## Project Structure

```
UsbAudioDac/
├── USB_Audio_DAC_1.0/             # Active CubeMX project
│   ├── Core/
│   │   ├── Src/
│   │   │   ├── main.c             # System init, clock config, I2S + USB init
│   │   │   ├── audio_i2s.c        # I2S DMA consumer (ring → stereo frames)
│   │   │   ├── ring_buffer.c      # Lock-free SPSC ring buffer
│   │   │   ├── audio_fft.c        # 1024-pt FFT → 12 perceptual band heights
│   │   │   ├── visualizer.c       # Boot splash + spectrum screen, layout
│   │   │   ├── st7735.c           # ST7735S SPI driver (owns SPI1)
│   │   │   ├── font8x8.h          # The one and only font
│   │   │   └── stm32f4xx_it.c     # Interrupt handlers + HAL callbacks
│   │   └── Inc/
│   │       ├── audio_i2s.h        # Buffer sizing, refill API
│   │       ├── ring_buffer.h      # Ring buffer struct and API
│   │       ├── audio_fft.h        # FFT sizes, band count, height cap
│   │       ├── st7735.h           # Panel geometry, bounds contract
│   │       └── visualizer.h       # Init + update API
│   ├── USB_DEVICE/
│   │   ├── App/
│   │   │   ├── usbd_audio_if.c    # USB audio → ring buffer bridge
│   │   │   └── usbd_desc.c        # USB device/configuration descriptors
│   │   └── Target/
│   │       ├── usbd_conf.c        # HAL PCD init, VBUS sensing disabled
│   │       └── usbd_conf.h        # USBD_AUDIO_FREQ = 48000
│   ├── tests/                     # Host unit tests (ring buffer, FFT, font)
│   ├── Drivers/                   # ST HAL + CMSIS (vendored)
│   ├── Middlewares/               # ST USB Device Library (Audio class)
│   ├── Makefile                   # GCC cross-compilation
│   ├── STM32F411xx_FLASH.ld       # Linker script (512K flash, 128K RAM — no CCM)
│   └── USB_Audio_DAC_1.0.ioc      # CubeMX project file
├── README.md                      # This file — overview, hardware, build, status
├── WIRING.md                      # Full MCU↔peripheral pin map + CubeMX config
├── DEBUGGING.md                   # Every bug hit, why it happened, how it was fixed
└── AGENTS.md                      # Project context for AI assistants
```

---

## Development Status

| Phase | Focus | Status |
|-------|-------|--------|
| 0 | Toolchain setup, LED blink, UART "Hello World" | ✅ Complete |
| 1 | Clock tree: HSE → PLL → 48 MHz SYSCLK, PLLI2S → 192 MHz I2S (exact 48 kHz) | ✅ Complete |
| 2 | I2S + DMA audio output (1 kHz test tone) | ✅ Complete |
| 3 | USB Audio Class 1.0 device — PC plays music to speaker | ✅ Complete |
| 3.5 | Reliability: stack bump, host unit tests, IWDG watchdog | ✅ Complete |
| 4 | Rotary encoder volume (software gain) + mute | ⏳ Planned |
| 5 | ST7735S TFT visualizer (12-band FFT + boot splash) | ✅ Complete |
| 6 | FreeRTOS integration (4 tasks: Audio, Display, Encoder, Debug) | ⏳ Planned |
| 7 | Polish, enclosure, final documentation | ⏳ Planned |

### Key Debugging Stories

The device enumerated correctly and played nothing, repeatedly. Four independent
bugs hid behind that one symptom — a bad VBUS sense line, a sample rate declared
in one place but generated in another, `USBD_AUDIO_Sync` never being called by
ST's library, and a +400 ppm clock error that produced periodic thuds minutes
into playback rather than silence. Later, a whole set of display bugs that were
invisible in code review: a transposed second font, a label clipped by rows the
panel physically cannot show, a status line garbling its own words, and a
`sqrt` approximation that drew a cone instead of a sun.

> **[DEBUGGING.md](DEBUGGING.md)** has the full story for each — symptom, why it
> happened, and the fix.

---

## Clock Configuration

| Clock | Frequency | Source |
|-------|-----------|--------|
| HSE | 25 MHz | External crystal (PH0/PH1) |
| SYSCLK | 48 MHz | PLL (M=25, N=384, P=DIV8) |
| USB | 48 MHz | PLLQ=8 (exact for Full-Speed USB) |
| I2S PLL (I2SCLK) | 192 MHz | PLLI2S (M=25, N=384, R=2) → VCO 384 MHz, 192 MHz I2SCLK |
| I2S sample rate | **48000 Hz exact** | HAL picks I2SDIV=62, ODD=1 → 192 MHz / (32 × 125). Zero drift vs the USB host. |
| AHB | 48 MHz | Prescaler = 1 |
| APB1 | 24 MHz | Prescaler = 2 |
| APB2 | 48 MHz | Prescaler = 1 |

> **Why 48 kHz and not 44.1 kHz:** 44 100 Hz is *mathematically unreachable* from a 25 MHz HSE
> through PLLI2S. `I2SCLK` would have to be an exact multiple of 1 411 200 Hz, but from 25 MHz,
> `I2SCLK = 25·N/(M·R)` with `N ≤ 432`, `M ≥ 13`, `R ≥ 2` — a ratio of small integers that can
> never land on that lattice (verified by exhaustive search over all legal M/N/R). The old
> 96 MHz setup gave **44 117.647 Hz** — a **+400 ppm** error against the declared 44 100 Hz,
> i.e. the I2S consumed 17.6 samples/s more than the host delivered, so the ring buffer slowly
> drained and every DMA refill came up short (zero-gap "taps", ~100/200 Hz).
> At 48 kHz the USB packets are a clean **96 bytes every millisecond** (no 44.1 kHz-style
> 88/90 "long frame" alternation) and the drain is exactly **0.000 samples/s**.
> The other two exact-48 kHz solutions (`N=384,R=5` → 76.8 MHz; `N=192,R=5` → 38.4 MHz) were
> rejected as low-divider / non-canonical. I2SCLK 192 MHz is within the datasheet's
> `fPLLI2S_OUT` max of 216 MHz and VCO 384 MHz within `fVCO_OUT` 100–432 MHz (Table 42).

---

## Roadmap

- [ ] **Phase 4**: Rotary encoder volume (software gain in `audio_i2s.c`), mute on button, wire `AUDIO_VolumeCtl_FS`
- [ ] **Phase 5**: ST7735S TFT real-time audio visualizer (level meter + spectrum bars)
- [ ] **Phase 6**: FreeRTOS with 4 concurrent tasks + inter-task communication
- [ ] **Phase 7**: Final polish, enclosure, full documentation
- [ ] **Stretch**: Stereo audio (2nd MAX98357A), CMSIS-DSP EQ effects, SD card logging, custom KiCad PCB

---

## Tools

| Tool | Purpose | Cost |
|------|---------|------|
| `arm-none-eabi-gcc` | Cross-compiler for Cortex-M4 | Free |
| STM32CubeMX | Peripheral configuration & code generation | Free |
| `st-flash` | Flash programming via ST-Link V2 | Free |
| VS Code | Code editor | Free |

## Reference Documents

This repo keeps four documents, each with one job:

| Document | Job |
|----------|-----|
| **README.md** (this file) | What it is, hardware, BOM, build/flash/verify, current architecture and status |
| **[WIRING.md](WIRING.md)** | Full MCU↔peripheral pin map, amp configuration, CubeMX settings |
| **[DEBUGGING.md](DEBUGGING.md)** | Every bug hit on this board: symptom, cause, fix, and what's still open |
| **[AGENTS.md](AGENTS.md)** | Operating notes for AI assistants — gotchas that cause silent failures, key files, deferred items |

Single-source-of-truth rule: the **clock tree is in README → Clock Configuration**,
the **pin map is in WIRING.md**, and the **gotchas are in AGENTS.md**. Code
comments reference those rather than restating numbers, so there is exactly one
place to update when a value changes.

---

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

The ST HAL drivers and USB Device Library included under `Drivers/` and `Middlewares/` are licensed by STMicroelectronics under their own [BSD-based license](USB_Audio_DAC_1.0/Drivers/STM32F4xx_HAL_Driver/LICENSE.txt).

---

## Acknowledgments

- [STMicroelectronics](https://www.st.com/) — STM32F411 HAL drivers and USB Device Library
- [MAX98357A](https://www.maximintegrated.com/en/products/analog/data-converters/dac-converters/MAX98357A.html) — I2S DAC + Class D amplifier
- [WeAct Black Pill](https://github.com/WeActStudio/MiniSTM32F4x1) — STM32F411CEU6 development board
- [STM32CubeMX](https://www.st.com/stm32cubemx) — Configuration and code generation tool
