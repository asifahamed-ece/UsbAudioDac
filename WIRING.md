# Wiring & Pin Configuration — STM32F411 USB Audio Player

Single source of truth for every MCU↔peripheral connection. Sections **A/B are wired and working**; **C/D/E are planned** (Phase 4–5) — the pins are free in the current firmware, so you can wire everything now, then enable the peripherals in CubeMX later.

---

## Pin Map (quick reference)

| MCU Pin | Function | Peripheral | AF | To |
|---|---|---|---|---|
| PA0 | DC (data/command) | GPIO out | — | ST7735S DC |
| PA1 | RST | GPIO out | — | ST7735S RESET |
| PA2 | TX | USART2 | AF7 | CH340 RX ⚠️ shared |
| PA3 | RX | USART2 | AF7 | CH340 TX |
| PA5 | SCK | SPI1 | AF5 | ST7735S SCL |
| PA7 | MOSI | SPI1 | AF5 | ST7735S SDA |
| PA11 | USB D- | USB OTG FS | — | USB-C (on-board) |
| PA12 | USB D+ | USB OTG FS | — | USB-C (on-board) |
| PB0 | CS | GPIO out | — | ST7735S CS |
| PB6 | Encoder A | TIM4_CH1 | AF2 | EC11 A |
| PB7 | Encoder B | TIM4_CH2 | AF2 | EC11 B |
| PB8 | Mute button | EXTI | — | EC11 SW |
| PB10 | BCLK | I2S2_CK | AF5 | MAX98357A BCLK |
| PB12 | WS/LRC | I2S2_WS | AF5 | MAX98357A LRC |
| PB15 | SDIN | I2S2_SD | AF5 | MAX98357A DIN |

---

## A) Audio Out — I2S2 → MAX98357A ✅ WIRED & WORKING

| MCU Pin | Function | → | MAX98357A | Wiring |
|---------|----------|---|-----------|--------|
| PB10 | I2S2_CK (AF5) | → | BCLK | jumper |
| PB12 | I2S2_WS (AF5) | → | LRC | jumper |
| PB15 | I2S2_SD (AF5) | → | DIN | jumper |
| GND | ground | → | GND | **must share MCU ground** |
| 5V | power | → | VIN | 5V = louder; 3V3 also works |

> I2S format: Philips standard, 16-bit data / 32-bit frame, 44.117 kHz. Verified playing.

## B) USB — OTG FS ✅ WIRED & WORKING

| MCU | → | Host |
|------|---|------|
| PA11 / PA12 (D-/D+) | on-board USB-C | PC |
| VBUS sensing | `vbus_sensing_enable = DISABLE` in `usbd_conf.c` | — |

## C) Rotary Encoder — Phase 4 ⚡ PLAN (wire now)

| EC11 Pin | → | MCU Pin | Function | Extra |
|----------|---|---------|----------|-------|
| A (pin 1) | → | PB6 | TIM4_CH1 | **10 kΩ pull-up → 3V3** * |
| B (pin 3) | → | PB7 | TIM4_CH2 | **10 kΩ pull-up → 3V3** * |
| C (pin 2) | → | GND | common | |
| SW (pin 4/5) | → | PB8 | EXTI (mute) | switch to **GND**; pull-up 10 kΩ → 3V3 or internal |

*Only if the module has no on-board pull-ups (bare EC11 typically needs them).

### CubeMX config (when Phase 4 starts)

```
TIM4: Clock Source = Internal
      PB6 = TIM4_CH1 (AF2), PB7 = TIM4_CH2 (AF2)
      Parameter Settings → Encoder Mode: Encoder Mode TI1 and TI2
      Prescaler 0, Auto-reload 0xFFFF (i.e. 65535, wraparound)
      No PWM generation on these pins. CNT read via TIM4->CNT.

PB8: GPIO_EXTI8, pull-up, falling edge (switch pulls low), EXTI8_IRQn
Encoder wiring debounce: small RC (10 kΩ + 100 nF) per channel is optional.
```

## D) ST7735S TFT — Phase 5 ⚡ PLAN (wire now)

| Display Pin | → | MCU Pin | Function | Notes |
|-------------|---|---------|----------|-------|
| VCC | → | 3V3 | power | **⚠️ 3.3 V ONLY — do NOT use 5V** |
| GND | → | GND | ground | |
| SCL (SCK) | → | PA5 | SPI1_SCK (AF5) | |
| SDA (MOSI) | → | PA7 | SPI1_MOSI (AF5) | |
| CS | → | PB0 | GPIO out | active low |
| DC | → | PA0 | GPIO out | 0 = command, 1 = data |
| RESET | → | PA1 | GPIO out | active low |
| LED | → | **3V3** | backlight | see conflict note ↓ |

### CubeMX config (when Phase 5 starts)

```
SPI1: Full-Duplex Master, 8-bit, MSB first, Mode 0 (CPOL=0, CPHA=0)
      Prescaler such that SCK ≤ 18 MHz → APB2 48 MHz / 4 = 12 MHz
      Software NSS (CS handled by GPIO).
      PA5 = SPI1_SCK (AF5), PA7 = SPI1_MOSI (AF5)
PA0, PA1, PB0 = GPIO_Output (DC, RST, CS)
```

## E) Debug UART — Optional (Phases 4–6)

| MCU Pin | Function | → | CH340 |
|---------|----------|---|-------|
| PA2 | USART2_TX (AF7) | → | RX |
| PA3 | USART2_RX (AF7) | → | TX |
| GND | ground | → | GND |

115200 8N1. Required only if you want `printf` debug output on-device.

---

## ⚠️ Conflicts & Rules

1. **PA2 shared:** TFT backlight (GPIO) **vs** USART2_TX. → Tie TFT **LED → 3V3**, keep PA2 for debug UART. (Loses software backlight toggle only.)
2. **ST7735S is 3.3 V only** — reverse polarity or 5 V kills the display.
3. **Common ground** — amplifier, display, MCU, and any debug adapter must share GND.
4. **MAX98357A power:** 5 V gives full volume; if you run it from the same USB supply, a separate 5 V/2 A charger reduces USB-bus noise.
5. **All unused pins are currently `GPIO_MODE_ANALOG`** in `main.c` (CubeMX default) — safe to run wires now; the peripheral init replaces modes on regeneration.

---

## ASCII Overview

```
                       ┌──────────────────────────┐
                       │  STM32F411CEU6 Black Pill │
   ST-Link V2 ──SWD──▶ │  SWDIO/SWCLK              │
                       │                           │
  USB-C (PC) ──▶ PA11/PA12  USB OTG FS             │
                       │                           │
        PB10 ──BCLK─▶ │  MAX98357A ─▶ (8 Ω speaker)│
        PB12 ──LRC──▶ │                           │
        PB15 ──SDIN─▶ │                           │
                       │                           │
        PB6  ◀─A──▶ EC11 ──B──▶ PB7               │   (Phase 4)
        PB8  ◀─SW── EC11 (GND), pull-ups →3V3      │
                       │                           │
        PA5  ◀─SCK─▶ │  ST7735S TFT (3.3V only)   │   (Phase 5)
        PA7  ◀─MOSI─▶│  CS=PB0, DC=PA0, RST=PA1,  │
                       │  LED→3V3                  │
                       │                           │
        PA2 ──TX──▶ CH340 RX, PA3 ◀─RX── CH340 TX  │   (optional debug)
                       └──────────────────────────┘
```