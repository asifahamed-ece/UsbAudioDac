# STM32F411 USB Audio Player

![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![STM32](https://img.shields.io/badge/MCU-STM32F411CEU6-blue.svg)
![HAL](https://img.shields.io/badge/Driver-STM32_HAL-orange.svg)
![USB Audio](https://img.shields.io/badge/USB-Audio%20Class%201.0-purple.svg)
![I2S](https://img.shields.io/badge/Protocol-I2S%20%2B%20DMA-red.svg)

A self-built USB speaker built on the **STM32F411CEU6 Black Pill**. When connected to a PC via USB, it enumerates as a standard USB Audio Class 1.0 device — no drivers needed. Audio data flows from the PC over USB, is decoded to analog via I2S, and played through a **MAX98357A** DAC + Class D amplifier driving a 4-ohm speaker.

A rotary encoder controls volume, and an ST7735S TFT displays a real-time audio visualizer.

<!-- ![Hardware Setup](photos/hardware_setup.jpg) -->
<!-- ![USB Audio Playing](photos/playing.jpg) -->

---

## Features

- **USB Audio Class 1.0** — Plug-and-play USB speaker on Linux, Windows, macOS
- **44.1 kHz / 16-bit / Mono** audio over USB isochronous endpoint
- **I2S + DMA** output to MAX98357A DAC + 3W Class D amplifier
- **Lock-free SPSC ring buffer** — 23 ms of audio headroom between USB and I2S
- **Rotary encoder volume control** with hardware DAC volume (Phase 4)
- **ST7735S TFT visualizer** with real-time audio spectrum (Phase 5)
- **FreeRTOS-based** multitasking architecture (Phase 6)
- **Low cost** — ~$10-13 in parts

---

## Hardware

| Component | Role |
|-----------|------|
| **STM32F411CEU6 Black Pill** | Main MCU (Cortex-M4, 100 MHz, 128 KB RAM, 512 KB Flash) |
| **MAX98357A** | I2S DAC + Class D amplifier module |
| **4-ohm speaker** | Audio output |
| **ST7735S 1.4" TFT** | Audio visualizer display (128x128, SPI) |
| **Rotary encoder (EC11)** | Volume control + mute button |
| **ST-Link V2** | Flash programmer and debugger |

### Bill of Materials

| Item | Qty | Purpose | Cost |
|------|-----|---------|------|
| STM32F411CEU6 Black Pill | 1 | Main MCU | ~$5 |
| MAX98357A I2S DAC + amp | 1 | DAC + amplifier | ~$3 |
| 4-ohm speaker | 1 | Audio output | ~$1 |
| ST7735S 1.4" TFT (SPI) | 1 | Visualizer display | ~$3 |
| Rotary encoder (EC11) | 1 | Volume control | ~$1 |
| ST-Link V2 | 1 | Flash + debug | ~$2-3 |
| USB-C cable | 1 | PC connection | ~$1 |
| Jumper wires + breadboard | 1 | Prototyping | ~$2 |
| Passive components (R, C) | — | RC filter, decoupling | ~$0.50 |
| **Total** | | | **~$10-13** |

> See [BOM.md](BOM.md) for the full bill of materials with optional upgrades and sourcing guidance.

---

## Wiring

### I2S Audio Output (MAX98357A)

| STM32 Pin | Function | MAX98357A Pin |
|-----------|----------|---------------|
| PB10 | I2S2 Clock (BCLK) | BCLK |
| PB12 | I2S2 Word Select (LRCLK) | LRC |
| PB15 | I2S2 Serial Data (SDIN) | DIN |
| GND | Ground | GND |
| 3V3 or 5V | Power | VIN |

### USB OTG Full-Speed

| STM32 Pin | Function |
|-----------|----------|
| PA11 | USB D- |
| PA12 | USB D+ |

### ST7735S TFT Display (Planned)

| STM32 Pin | Display Pin | Function |
|-----------|-------------|----------|
| PA5 | SCK | SPI Clock |
| PA7 | MOSI | SPI Data |
| PB0 | CS | Chip Select |
| PA0 | DC | Data/Command |
| PA1 | RST | Reset |
| PA2 | LED | Backlight |

### Rotary Encoder (Planned)

| STM32 Pin | Encoder Pin | Function |
|-----------|-------------|----------|
| PB6 | A | TIM4 Encoder Ch A |
| PB7 | B | TIM4 Encoder Ch B |
| PB8 | Button | EXTI (mute) |

---

## Audio Pipeline

```
PC (USB audio source)
    │
    │  USB Full-Speed (12 Mbps, 44.1 kHz 16-bit mono)
    ▼
STM32F411 Black Pill
    │
    ├── USB OTG FS ─────── receives 88-byte packets (44 samples) every 1 ms
    │       │
    │       ▼
    │   usbd_audio_if.c ── AUDIO_CMD_PLAY → RingBuffer_Write()
    │       │
    │       ▼
    │   ring_buffer.c ──── SPSC ring, 1024 int16 = 23 ms headroom
    │       │
    │       ▼
    │   audio_i2s.c ────── RefillHalfA/B: mono → L+R stereo duplication
    │       │
    │       ▼
    ├── I2S2 + DMA1 ────── circular DMA, ping-pong halves (10 ms each)
    │       │
    │       ▼
    │   MAX98357A ──────── I2S DAC + Class D amp
    │       │
    │       ▼
    └──▶ 4-ohm Speaker
```

---

## Build & Flash

### Prerequisites

- `arm-none-eabi-gcc` (tested with v16.2.0)
- `st-flash` (ST-Link V2 flash tool)
- `make`
- ST-Link V2 programmer connected to the Black Pill

### Build

```bash
cd USB_Audio_DAC_1.0
make
```

### Flash

```bash
cd USB_Audio_DAC_1.0
st-flash write build/USB_Audio_DAC_1.0.bin 0x08000000
```

### Clean

```bash
make clean
```

### Verify (Linux)

After flashing, connect the Black Pill to your PC via USB-C:

```bash
# Check USB enumeration
lsusb -v | grep -A 10 "Audio"

# Test with a 1 kHz sine tone
speaker-test -D plughw:2,0 -c 1 -r 44100 -t sine -f 1000

# Play a WAV file
aplay -D plughw:2,0 your_audio.wav
```

---

## Project Structure

```
UsbAudioDac/
├── USB_Audio_DAC_1.0/           # Active CubeMX project
│   ├── Core/
│   │   ├── Src/
│   │   │   ├── main.c           # System init, clock config, I2S + USB init
│   │   │   ├── audio_i2s.c      # I2S DMA consumer (ring → stereo frames)
│   │   │   ├── ring_buffer.c    # Lock-free SPSC ring buffer
│   │   │   └── stm32f4xx_it.c   # Interrupt handlers + HAL callbacks
│   │   └── Inc/
│   │       ├── audio_i2s.h      # Buffer sizing, refill API
│   │       └── ring_buffer.h    # Ring buffer struct and API
│   ├── USB_DEVICE/
│   │   ├── App/
│   │   │   ├── usbd_audio_if.c  # USB audio → ring buffer bridge
│   │   │   └── usbd_desc.c      # USB device/configuration descriptors
│   │   └── Target/
│   │       ├── usbd_conf.c      # HAL PCD init, VBUS sensing disabled
│   │       └── usbd_conf.h      # USBD_AUDIO_FREQ = 44100
│   ├── Drivers/                 # ST HAL + CMSIS (vendored)
│   ├── Middlewares/              # ST USB Device Library (Audio class)
│   ├── Makefile                 # GCC cross-compilation
│   ├── STM32F411xx_FLASH.ld    # Linker script (512K flash, 128K RAM)
│   └── USB_Audio_DAC_1.0.ioc   # CubeMX project file
├── IMPLEMENTATION_PLAN.md       # Full 8-phase plan with task breakdowns
├── PROGRESS.md                  # Phase-by-phase working log + debugging stories
├── BOM.md                       # Bill of materials + pinout reference
└── CLAUDE.md                    # Project context for AI assistants
```

---

## Development Status

| Phase | Focus | Status |
|-------|-------|--------|
| 0 | Toolchain setup, LED blink, UART "Hello World" | ✅ Complete |
| 1 | Clock tree: HSE → PLL → 48 MHz SYSCLK, PLLI2S → 96 MHz I2S | ✅ Complete |
| 2 | I2S + DMA audio output (1 kHz test tone) | ✅ Complete |
| 3 | USB Audio Class 1.0 device — PC plays music to speaker | ✅ Complete |
| 4 | Rotary encoder volume control + hardware DAC volume | ⏳ Planned |
| 5 | ST7735S TFT audio visualizer + spectrum analyzer | ⏳ Planned |
| 6 | FreeRTOS integration (4 tasks: Audio, Display, Encoder, Debug) | ⏳ Planned |
| 7 | Polish, documentation, README | 🔄 In Progress |

### Key Debugging Stories

Phase 3 had three silent-failure bugs that each took an evening to track down — all caused the device to enumerate correctly but play no audio:

1. **VBUS sensing** — Black Pill's PA9/VBUS line doesn't reliably trigger OTG FS comparator; fix was disabling `vbus_sensing_enable`
2. **Sample rate mismatch** — Hardcoded `48000U` in `usbd_conf.h` overrode the .ioc's 44100 setting, causing ring over/underrun
3. **Missing `USBD_AUDIO_Sync` call** — ST's library exports this function but never calls it internally; user must invoke it from I2S DMA callbacks

> See [PROGRESS.md](PROGRESS.md) for the full debugging story and lessons learned.

---

## Clock Configuration

| Clock | Frequency | Source |
|-------|-----------|--------|
| HSE | 25 MHz | External crystal (PH0/PH1) |
| SYSCLK | 48 MHz | PLL (M=25, N=384, P=DIV8) |
| USB | 48 MHz | PLLQ=8 (exact for Full-Speed USB) |
| I2S PLL | 96 MHz | PLLI2S (M=25, N=192, R=2) |
| AHB | 48 MHz | Prescaler = 1 |
| APB1 | 24 MHz | Prescaler = 2 |
| APB2 | 48 MHz | Prescaler = 1 |

---

## Roadmap

- [ ] **Phase 4**: Rotary encoder volume control with DAC-based hardware volume
- [ ] **Phase 5**: ST7735S TFT real-time audio visualizer with spectrum analysis
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
