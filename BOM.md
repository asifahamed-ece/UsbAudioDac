# Bill of Materials — STM32F411 USB Audio Player

This file lists all hardware components needed for the project. Items marked ✅ are already owned; items marked ⏳ need to be purchased.

---

## Status Legend
- ✅ **Already owned**
- ⏳ **Need to purchase**
- ❓ **Optional / TBD**

---

## Core Components (Required)

| Status | Item | Qty | Purpose | Approx Cost |
|--------|------|-----|---------|-------------|
| ✅ | STM32F411CEU6 Black Pill | 1 | Main MCU | $0 (owned) |
| ✅ | MAX98357A I2S DAC + Class D amp module | 1 | DAC + 3W amplifier | $0 (owned) |
| ⏳ | PAM8403 amp module | 1 | Headphone-friendly second amp stage | $0 (owned) |
| ✅ | 4Ω small speaker (mono) | 1 | Audio output | $0 (owned) |
| ✅ | ST-Link V2 programmer | 1 | Flash + debug firmware | $2-3 |
| ⏳ | USB-Serial adapter (CH340 or CP2102) | 1 | Debug serial output via USART | $1-2 |

## Output & Display Components (Required)

| Status | Item | Qty | Purpose | Approx Cost |
|--------|------|-----|---------|-------------|
| ✅ | ST7735S 1.4" TFT LCD (RGB, SPI) | 1 | Audio visualizer / status display (full color) | $0 (owned) |
| ⏳ | Rotary encoder with push button (EC11 or similar) | 1 | Volume control + mute | $1 |
| ⏳ | 3.5mm TRS jack (or use speaker direct) | 1 | Optional headphone output | <$1 |

**Note:** The SSD1306 OLED (I2C) has been replaced with ST7735S TFT (SPI) for full-color display of audio visualizer with spectrum analysis.

## Passive Components (Required)

| Status | Item | Qty | Purpose | Approx Cost |
|--------|------|-----|---------|-------------|
| ⏳ | Resistor 1 kΩ (1/4W) | 1 | RC low-pass for DAC output (volume) | <$0.10 |
| ⏳ | Resistor 10 kΩ (1/4W) | 1 | RC low-pass for DAC output (volume) | <$0.10 |
| ⏳ | Capacitor 100 nF ceramic | 2 | RC low-pass + power decoupling | <$0.10 |
| ⏳ | Capacitor 10 µF electrolytic | 1 | RC low-pass filter cap | <$0.10 |
| ⏳ | Capacitor 100 µF electrolytic | 2 | Power decoupling for MAX98357A + PAM8403 | <$0.20 |

## Connectors & Wiring (Required)

| Status | Item | Qty | Purpose | Approx Cost |
|--------|------|-----|---------|-------------|
| ✅ | USB-C cable (data + power) | 1 | Connect Black Pill to PC for USB audio | $1-2 |
| ✅ | Mini-USB cable (for ST-Link V2) | 1 | Programmer to PC | $1 (or use existing) |
| ✅ | Jumper wires (M-M, M-F, F-F) | ~20 | Prototyping wiring | $1-2 |
| ✅ | Breadboard or perfboard | 1 | Prototyping platform | $1-2 |

## Power Components (Optional but Recommended)

| Status | Item | Qty | Purpose | Approx Cost |
|--------|------|-----|---------|-------------|
| ✅ | 5V 2A USB power supply (phone charger) | 1 | Separate power for amp stage to avoid USB noise | $0 (use existing) |

---

## Optional / Future Upgrades

| Status | Item | Qty | Purpose | Approx Cost |
|--------|------|-----|---------|-------------|
| ❓ | 2nd MAX98357A module | 1 | Stereo audio upgrade | $3-5 |
| ❓ | 2nd 4Ω speaker | 1 | Stereo (requires 2nd MAX98357A) | $2 |
| ✅ | SD card module (SPI) | 1 | Audio statistics logging | $1-2 |
| ❓ | TPA6120 headphone amp module | 1 | Higher-quality headphone output | $3-5 |
| ❓ | Enclosure (3D printed or bought) | 1 | Project presentation | $0-10 |

---

## Cost Summary

### Required Items Only
| Category | Cost |
|----------|------|
| Core components | $0 (all owned except programmer + serial adapter) |
| ST-Link V2 | $2-3 |
| USB-Serial adapter | $1-2 |
| Display + encoder | $3 |
| Passives | ~$0.50 |
| Connectors & wiring | $3-4 |
| **Total required** | **~$10-13** |

### Optional Stretch Goals
| Category | Cost |
|----------|------|
| Stereo upgrade (2nd MAX + 2nd speaker) | $5-7 |
| SD card module | $1-2 |
| TPA6120 headphone amp | $3-5 |
| Enclosure | $0-10 |
| **Total stretch goals** | **$9-24** |

---

## Where to Buy

- **AliExpress** — Cheapest, 2-4 week shipping (China → India/wherever)
- **Amazon** — Faster shipping, 1-2x the price
- **LCSC / Digikey / Mouser** — For genuine components, more reliable
- **Local electronics shops** — For passive components and connectors

---

## Display Pinout Reference (ST7735S 1.44" 128×128 TFT)

**⚠️ Important:** This display runs on **3.3V only** — do NOT connect to 5V.

| Display Pin | Function | STM32F411 Pin | Black Pill Header |
|-------------|----------|---------------|-------------------|
| VCC | Power 3.3V | 3V3 | 3V3 |
| GND | Ground | GND | GND |
| CS | Chip Select (active low) | **PB0** | B0 (changed from PA4 to avoid DAC1 conflict) |
| RESET | Hardware reset (active low) | PA1 | A1 |
| DC | Data/Command (0=cmd, 1=data) | PA0 | A0 |
| SDA (MOSI) | SPI Data | **PA7 (SPI1_MOSI)** | A7 |
| SCL (SCK) | SPI Clock | **PA5 (SPI1_SCK)** | A5 |
| LED | Backlight (anode) | PA2 | A2 (optional, can tie to 3V3) |

**Connections used:** SPI1 (PA5, PA7) + 5 GPIO pins (PA0, PA1, PA2, PB0) = 7 wires + 2 power

---

## Recommended Shopping List (Minimum Order)

If buying all at once on AliExpress, this should be ~$10-13 total:

1. 1× ST-Link V2 (metal case, clone OK)
2. 1× USB-Serial adapter (CH340 module is cheapest)
3. ~~1× SSD1306 OLED 0.96" I2C (blue or white)~~ — Replaced with ST7735S 1.4" TFT (already owned)
4. 1× Rotary encoder with button (EC11 type, 5-pin or 7-pin)
5. 1× Set of jumper wires (M-M, M-F, F-F combo pack)
6. 1× Breadboard 830 points
7. 1× Resistor assortment (1kΩ, 10kΩ)
8. 1× Capacitor assortment (100nF, 10µF, 100µF)
9. 1× 3.5mm TRS jack breakout
10. 1× USB-C cable (data, not just charging)
11. 1× Mini-USB cable (for ST-Link)
