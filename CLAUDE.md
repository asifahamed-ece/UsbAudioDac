# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## Project Overview

**STM32F411 USB Audio Player** — A self-built USB speaker built on the STM32F411CEU6 Black Pill. It enumerates as a USB Audio Class 1.0 device, decodes audio to analog via I2S, and drives a speaker through the MAX98357A DAC/amp. A rotary encoder controls volume, and a ST7735S TFT displays an audio visualizer. Firmware is FreeRTOS-based.

Target hardware: **STM32F411CEU6 Black Pill** (Cortex-M4, 100 MHz, 128 KB RAM, 512 KB Flash, 64 KB CCMRAM)

---

## Build Commands

The CubeMX-generated project lives in `USB_Audio_DAC_1.0/`. Build and flash from that directory.

### Full build
```bash
cd USB_Audio_DAC_1.0
make
```

### Flash to board (via ST-Link V2)
```bash
cd USB_Audio_DAC_1.0
st-flash write build/USB_Audio_DAC_1.0.bin 0x08000000
```

### Erase and reset
```bash
st-flash erase 0x08000000 512K
st-flash reset
```

### Build with verbose output
```bash
cd USB_Audio_DAC_1.0
make V=1
```

### Clean build artifacts
```bash
cd USB_Audio_DAC_1.0
make clean
```

**Toolchain:** `arm-none-eabi-gcc` (Arch Repository, version 16.2.0)
**Flash tools:** `st-flash` (ST-Link V2) and `openocd` are available

---

## Architecture

### Phased Implementation (8 phases)

The firmware is implemented in 8 sequential phases. Each phase builds on the previous one and produces a demonstrable deliverable.

| Phase | Focus | Key Peripherals |
|-------|-------|-----------------|
| 0 | Toolchain setup, LED blink, "Hello World" UART | GPIO, USART2 |
| 1 | Clock tree: HSE→PLL→48 MHz SYSCLK, PLLI2S→96 MHz I2S clock | RCC, PLL, PLLI2S |
| 2 | I2S+DMA audio output (1 kHz test tone) | I2S2, DMA1, GPIO alt func |
| 3 | USB Audio Class 1.0 device — PC sends music to board | USB OTG FS, NVIC |
| 4 | Rotary encoder volume control + hardware DAC volume | TIM4 (encoder mode), DAC1, EXTI |
| 5 | ST7735S TFT audio visualizer + spectrum analyzer | SPI1, GPIO |
| 6 | FreeRTOS integration (4 tasks: Audio, Display, Encoder, Debug) | SysTick, queues, semaphores |
| 7 | Polish, documentation, README | — |

### Source Structure

```
USB_Audio_DAC_1.0/
├── Core/
│   ├── Src/
│   │   ├── main.c              — system init, clock config, I2S + USB init
│   │   ├── audio_i2s.c         — Phase 2-3: I2S DMA consumer (ring → stereo frames)
│   │   ├── ring_buffer.c       — Phase 3: lock-free SPSC ring buffer
│   │   ├── stm32f4xx_it.c      — Interrupt handlers + HAL I2S callbacks
│   │   ├── stm32f4xx_hal_msp.c — HAL MSP init (I2S2 GPIO, DMA, PLLI2S config)
│   │   ├── syscalls.c          — Newlib system call stubs
│   │   ├── sysmem.c            — Newlib memory allocation stubs
│   │   └── system_stm32f4xx.c  — CMSIS system initialization
│   └── Inc/
│       ├── audio_i2s.h         — I2S DMA buffer sizing and refill API
│       ├── main.h              — Main header with HAL includes
│       ├── ring_buffer.h       — SPSC ring buffer API and struct definition
│       ├── stm32f4xx_hal_conf.h — HAL configuration
│       └── stm32f4xx_it.h      — Interrupt handler prototypes
├── USB_DEVICE/
│   ├── App/
│   │   ├── usbd_audio_if.c     — USB audio → ring buffer bridge
│   │   └── usbd_desc.c         — USB device/configuration descriptors
│   └── Target/
│       ├── usbd_conf.c         — HAL PCD MSP init, VBUS sensing disabled
│       └── usbd_conf.h         — USBD_AUDIO_FREQ = 44100
├── Drivers/                    # ST HAL + CMSIS (vendored)
├── Middlewares/                 # ST USB Device Library (Audio class)
├── Makefile                    # GCC cross-compilation
├── STM32F411xx_FLASH.ld       # Linker script (512K flash, 128K RAM)
└── USB_Audio_DAC_1.0.ioc      # CubeMX project file
```

### Hardware Architecture

```
PC (USB audio source)
    │
    │  USB Full-Speed (12 Mbps, 48 kHz 16-bit mono)
    ▼
STM32F411 Black Pill
    ├── USB OTG FS          → receives audio packets
    ├── I2S2 + DMA1         → outputs PCM to MAX98357A
    ├── SPI1 (PA5/PA7)      → drives ST7735S TFT display
    ├── TIM4 (encoder mode) → reads rotary encoder (PB6/PB7)
    ├── DAC1 (PA4)          → hardware volume control → PAM8403
    └── USART2 (PA2/PA3)    → debug serial @ 115200 baud
         │
         ├──▶ MAX98357A → 4Ω speaker
         ├──▶ ST7735S 1.4" TFT (128×128, SPI)
         └──▶ Rotary encoder (volume + mute)
```

### Key Pin Assignments

| Peripheral | Pins | Notes |
|-----------|------|-------|
| USB DM/DP | PA11/PA12 | USB OTG FS Device |
| I2S2 WS/CK/SD | PB12/PB13/PB15 | I2S2 alt func 5 |
| SPI1 SCK/MOSI | PA5/PA7 | ST7735S display |
| Display CS/DC/RST/LED | PB0/PA0/PA1/PA2 | ST7735S control |
| Encoder A/B | PB6/PB7 | TIM4 encoder mode |
| Encoder button | PB8 | EXTI |
| DAC1 OUT | PA4 | Hardware volume → RC filter |
| USART2 TX/RX | PA2/PA3 | Debug serial 115200 |

### Reference Documents

- **RM0383** — STM32F411xC/E Reference Manual (clock tree: Ch.6, USB: Ch.31, I2S: Ch.28, DMA: Ch.9, TIM: Ch.13-14)
- **IMPLEMENTATION_PLAN.md** — Full phase-by-phase breakdown with tasks, deliverables, and RM chapter references
- **BOM.md** — Bill of materials, pinout reference for ST7735S, shopping list

### Clock Configuration

- **HSE:** 25 MHz crystal on PH0/PH1
- **SYSCLK:** 48 MHz (PLL: PLLM=25, PLLN=384, PLLP=DIV8 → 25/25×384/8=48 MHz)
- **USB clock:** 48 MHz (PLLQ=8 → exact for Full-Speed USB)
- **I2S clock:** 48 MHz from PLLI2S (PLLM=25, PLLN=384) — drives I2S2 via `I2S_CLOCK_PLL`
- **AHB / APB1 / APB2:** 48 MHz / 24 MHz (DIV2) / 48 MHz (DIV1)
- **FLASH_LATENCY:** 1 WS (for 48 MHz at 2.7–3.6 V)
- **FLASH_LATENCY:** 1 WS for 60 MHz at 2.7-3.6 V
- **Note:** the early "100 MHz SYSCLK + 12.288 MHz external MCLK" plan was dropped because (a) PI0/I2S_CKIN is not exposed on the F411 CEU6 UFQFPN48, and (b) PLLI2S at 48 MHz gives the audio quality we need

---

## Notes

- The active project is the CubeMX-generated `USB_Audio_DAC_1.0/` (not a hand-rolled register-level project). It uses ST's HAL drivers and a CubeMX-generated Makefile.
- Currently mid-Phase 2: I2S2 + DMA1 plays a 1 kHz sine through MAX98357A. Phase 3 (USB Audio Class 1.0) is the next step.
- See `PROGRESS.md` for the phase-by-phase working log, including the 344 Hz → 1 kHz debugging story.
- See `IMPLEMENTATION_PLAN.md` for the full 8-phase plan with task breakdowns and RM0383 chapter references.
