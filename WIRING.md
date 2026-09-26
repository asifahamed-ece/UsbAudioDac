# Wiring & Pin Configuration — STM32F411 USB Audio Player

Single source of truth for every MCU↔peripheral connection. Sections **A/B are wired and working**; **C/D/E are planned** (Phase 4–5) — the pins are free in the current firmware, so you can wire everything now, then enable the peripherals in CubeMX later.

---

## Pin Map (quick reference)

| MCU Pin | Function | Peripheral | AF | To |
|---|---|---|---|---|
| PA0 | DC (data/command) | GPIO out | — | ST7735S DC |
| PA1 | RST | GPIO out | — | ST7735S RESET |
| PA2 | TX | USART2 | AF7 | CH340 RX — *not enabled, see §E* |
| PA3 | RX | USART2 | AF7 | CH340 TX — *not enabled, see §E* |
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
| PB12 | I2S2_WS (AF5) | → | LRC/LRCLK | jumper |
| PB15 | I2S2_SD (AF5) | → | DIN | jumper |
| — | GPIO out (opt.) | → | SD | **optional mute** (see ↓) |
| GND | ground | → | GND | **must share MCU ground** |
| 5V | power | → | VIN | 2.5–5.5 V; 5 V = full power |

> I2S format: Philips standard, 16-bit data / 32-bit frame, **48 000 Hz exact**, **no MCLK used** (this amp needs no MCLK — leave any MCLK output disconnected). Verified playing.
> *(An earlier build ran this at 44 117 Hz; that was discarded because 44.1 kHz is unreachable exactly from the 25 MHz HSE — see README → Clock Configuration for the +400 ppm analysis and the 192 MHz PLLI2S that replaced it.)*

### MAX98357A module pinout & configuration reference

| Module pin | Function | Notes |
|------------|----------|-------|
| **LRC** | Frame clock / word select | Driven by PB12. **Not configurable.** I2S convention: LRCLK **low = left** word, high = right |
| **BCLK** | Bit clock | Driven by PB10 |
| **DIN** | Data in | Driven by PB15 (both L+R words on this one pin) |
| **SD** (also labelled "SCK" on clones) | Shutdown **+ channel select** — strapped at power-up | Internal 100 kΩ pull-down inside the amp (see table ↓) |
| **GAIN** (also "GAIN_SLOT") | Amplifier gain — strapped at power-up | 5 levels; part number is read, **not** register-set |
| **VIN** | Power 2.5–5.5 V | 5 V → 1.8 W @ 8 Ω (10% THD); 3.3 V → ~0.8 W |
| **GND** | Ground | Share with MCU ground |
| Speaker (OUTP/OUTN) | Bridge-tied class-D output | **Moving-coil ≥ 4 Ω only** — never use it as a pre-amp |

#### GAIN pin → gain (I2S mode, from datasheet Table 1 / JU5 + Adafruit)

| GAIN connection | Gain | Use case |
|-----------------|------|----------|
| GAIN → GND | **12 dB** | Good "loud + headroom" baseline. **This is what this build uses — see note below.** |
| GAIN → nothing (float) | **9 dB (factory default)** | Datasheet default. Was the original advice here; **discarded — see note below.** |
| GAIN → GND via 100 kΩ | 15 dB | Maximum |
| GAIN → VIN | 6 dB | Quiet source, low-noise |
| GAIN → VIN via 100 kΩ | 3 dB | Minimum |

> **MEASURED ON THIS BOARD — GAIN must be tied, not left floating.**
> The advice used to be "leave GAIN floating for the cleanest 9 dB". In practice
> a floating GAIN produced audible low-level distortion (a ~200/300 Hz buzzy
> character under sustained tone) that no amount of firmware chasing could
> remove. A generated-tone isolation test in `audio_i2s.c` (`dbg_bypass_usb`)
> proved the I2S → DMA → amp path was clean, which pointed at the amp; tying
> **GAIN to GND** removed the artifact completely. The datasheet's
> "float = 9 dB default" is a spec, not a measurement — on this module the
> floating pin is evidently picking up noise. If you hear that buzzy
> character, check GAIN before looking at the firmware.

#### SD (SD_MODE) pin → shutdown & channel (datasheet Table 5, trip points B0/B1/B2)

| Voltage on SD | Result |
|---------------|--------|
| < 0.16 V (tie SD → GND) | **Shutdown** (µA quiescent — hardware mute) |
| 0.16 – 0.77 V | **(L+R)/2** mono mix |
| 0.77 – 1.4 V | **Right** word only |
| > 1.4 V (tie SD → 3V3/5V) | **Left** word only — recommended |
| Floating | NOT safe: internal 100 kΩ pull-down drags SD near GND → shutdown/flicker. On Adafruit-style boards a 1 MΩ pull-up to VIN biases it into mono-mix. |

#### Practical wiring for this project

- **Tie GAIN to GND** (12 dB) — see the measured note in the GAIN table above. Leaving it floating is what caused the residual distortion. Leave **SD** exactly as the board shipped; it already plays.
- Optional hardware **mute / power-save (Phase 4+):** drive SD from an MCU GPIO (push-pull, 3.3 V logic, no resistor needed):
  `GPIO high → left-channel play`, `GPIO low → shutdown`. 3.3 V is safely above the 1.4 V B2 trip point, 0 V below the 0.16 V B0 point. Pick a spare pin (e.g. PB9). Don't tie SD to a clock pin — it is *not* an I2S clock.
- **Channel select doesn't affect audio here** — the firmware sends identical L/R samples per frame, so left / right / mono all sound identical. Left is the convention to use.
- Gain vs. volume: I2S full-scale is fixed, so gain just sets how loud that is. GAIN is hardware (12 dB); let the Phase 4 **software volume** scale the samples.
- Verify what *your* module strapped: with the board powered off, measure GAIN/SD continuity to VIN/GND (R×1k) — or just leave it, since it works.

## B) USB — OTG FS ✅ WIRED & WORKING

| MCU | → | Host |
|------|---|------|
| PA11 / PA12 (D-/D+) | on-board USB-C | PC |
| VBUS sensing | `vbus_sensing_enable = DISABLE` in `usbd_conf.c` | — |

## C) Rotary Encoder — Phase 4 ⚡ PLAN (not built yet)

> **Nothing here is wired, and no encoder behaviour exists in firmware.**
> `AUDIO_VolumeCtl_FS` is a no-op (`UNUSED(vol); return USBD_OK;`) and there is no
> software gain in the sample path, so the device has **no volume control at
> all** today. This section is the plan for when you start Phase 4, not a
> description of the current build.

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

## D) ST7735S TFT — Phase 5 ✅ WIRED & WORKING

> **Built and verified on the glass** (merged to `main` via PR #4). Note that
> SPI1 is *not* a CubeMX peripheral here — `st7735.c` owns SPI1 entirely and
> initialises it itself, so there is no `hspi1` in the `.ioc`. The settings
> below are what the driver applies, not a `.ioc` diff to make.

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

### SPI1 config (as applied by `st7735.c`)

```
SPI1: Full-Duplex Master, 8-bit, MSB first, Mode 0 (CPOL=0, CPHA=0)
      Prescaler such that SCK ≤ 18 MHz → APB2 48 MHz / 4 = 12 MHz
      Software NSS (CS handled by GPIO).
      PA5 = SPI1_SCK (AF5), PA7 = SPI1_MOSI (AF5)
PA0, PA1, PB0 = GPIO_Output (DC, RST, CS)
```

## E) Debug UART — NOT WIRED UP (no UART in the firmware)

| MCU Pin | Function | → | CH340 |
|---------|----------|---|-------|
| PA2 | USART2_TX (AF7) | → | RX |
| GND | ground | → | GND |

> **This is aspirational, not current.** As of the last build there is **no
> USART2 in the firmware**: `MX_USART2_UART_Init` is never called, the UART HAL
> driver is not in the `Makefile`, and `USBD_DEBUG_LEVEL` is `0`, which compiles
> every `USBD_ErrLog` / `USBD_DbgLog` call to nothing. So an adapter plugged in
> here would receive no bytes.
>
> On-device `printf` was dropped in favour of the IWDG watchdog plus SWD/GDB for
> debugging. Wiring this up later means: enable USART2 in the `.ioc`, add the UART
> driver to `C_SOURCES`, retarget `_write` to the UART, and raise
> `USBD_DEBUG_LEVEL`. Until then, **don't buy a CH340/CP2102 for this project.**

---

## ⚠️ Conflicts & Rules

1. **PA2 shared:** TFT backlight (GPIO) **vs** USART2_TX. → Tie TFT **LED → 3V3**, as wired today. The conflict is currently moot because USART2 is unused (§E), but keep the backlight tied if you ever enable it, or you lose the software backlight toggle.
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
        PA5  ◀─SCK─▶ │  ST7735S TFT (3.3V only)   │   (Phase 5, working)
        PA7  ◀─MOSI─▶│  CS=PB0, DC=PA0, RST=PA1,  │
                       │  LED→3V3                  │
                       │                           │
        PA2 ──TX──▶ CH340 RX, PA3 ◀─RX── CH340 TX  │   (NOT enabled — no UART in fw)
                       └──────────────────────────┘
```