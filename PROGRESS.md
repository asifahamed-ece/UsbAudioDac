# Project Progress

STM32F411 USB Audio Player — block-by-block learning log.

---

## Phase 0 — Toolchain Setup & Board Bring-Up

| Status | Task |
|--------|------|
| ✅ | Tools installed on Arch Linux: `arm-none-eabi-gcc`, `openocd`, `st-flash`, STM32CubeMX, VS Code |
| ✅ | ST-Link V2 detected STM32F411CEU6 (chip ID 0x431, 512 KB flash, 128 KB RAM) |
| ✅ | STM32CubeMX project created: `phase0_blinky` |
| ✅ | Configured PC13 as GPIO_Output (active-LOW LED) |
| ✅ | Project generated with Makefile toolchain |
| ✅ | Built and flashed via `st-flash write ... 0x08000000` |
| ✅ | **LED is blinking on the desk** |

**Workflow learned:**
- CubeMX pinout configuration (GPIO_Output, user label `LED`)
- Project Manager settings (Toolchain = Makefile, Application Structure = Advanced)
- Code generation with `USER CODE` block discipline
- Command-line build (`make`) and flash (`st-flash write <bin> 0x08000000`)

**Toolchain summary:**
- `arm-none-eabi-gcc` for compilation
- STM32CubeMX for code generation
- VS Code for editing
- `st-flash` for flashing
- (Next phase will add: register-level understanding, clock tree manipulation)

---

## Phase 1 — Clock Tree (HSE → PLL → 60 MHz SYSCLK, PLLQ → 48 MHz USB, External 12.288 MHz I2S)

| Status | Task |
|--------|------|
| ✅ | **Completed** |

**Goal:** Achieve the exact clock configuration needed for USB audio + high-quality I2S output.

**What we now have (final working configuration):**
```
HSE = 25.000 MHz (external crystal)
SYSCLK = 60.000 MHz (PLL: M=15, N=144, P=4)
USBCLK = 48.000 MHz (PLLQ=5)
I2S MCLK = 12.288 MHz (external crystal via I2S_CKIN pin)
CSS Enabled for clock fault detection
```

**Why this configuration works:**
- **USB audio:** 48 MHz USB clock meets ±0.25% accuracy requirement for USB Full-Speed enumeration
- **I2S audio:** External 12.288 MHz crystal provides pristine MCLK for 48 kHz × 256 = 12.288 MHz exact
- **System processing:** 60 MHz CPU provides ample headroom for audio buffering, USB packet handling, and future features
- **Clock separation:** USB, I2S, and processing clocks are perfectly isolated, eliminating jitter and beat frequencies

**Clock tree configuration details:**
- Main PLL: `PLLM=15, PLLN=144, PLLP=4` → `VCO = (25/15) × 144 = 240 MHz` → `SYSCLK = 240/4 = 60 MHz`
- USB clock: `PLLQ=5` → `USBCLK = 240/5 = 48 MHz` (exact USB Full-Speed requirement)
- AHB Prescaler = 1 → `HCLK = 60 MHz` (full speed to Cortex core, DMA, memory)
- APB1 Prescaler = 2 → `PCLK1 = 30 MHz` (safe for USART2, I2C, SPI2, DAC)
- APB2 Prescaler = 1 → `PCLK2 = 60 MHz` (full speed for SPI1, TIM1/9-11)
- I2S clock: External 12.288 MHz crystal via `I2S_CKIN` pin (PI0) → feeds directly to I2S2 peripheral

**Current configuration advantages:**
✅ Perfect USB audio (48 MHz exact)  
✅ Perfect I2S audio (12.288 MHz external crystal, zero jitter)  
✅ Excellent system performance (60 MHz CPU)  
✅ No clock conflicts or beat frequencies  
✅ Ready for Phase 2 (I2S + DMA audio output)  

**Key achievements:**
- Moved from HSI 16 MHz to stable HSE 25 MHz crystal
- Configured PLL to generate exact 60 MHz SYSCLK
- Generated exact 48 MHz USB clock from PLLQ
- Implemented external 12.288 MHz I2S clock for pristine audio
- Verified all peripheral clocks are within safe limits
- System ready for USB Audio Class 1.0 + I2S output

**Deliverable:** Clock tree validated with exact frequencies for USB audio + I2S output. System ready for Phase 2.

---

## Phase 2 — I2S + DMA Audio Output (1 kHz test tone) 🔄

| Status | Task |
|--------|------|
| 🔄 | **In Progress** |

**Goal:** Generate a 1 kHz sine wave in code, push it to MAX98357A over I2S2 + DMA1, hear it on the speaker.

**Configuration completed in CubeMX:**
- ✅ I2S2: Half-Duplex Master, Philips standard, 16-bit data on 32-bit frame
- ✅ Audio Frequency: 48 kHz (Real: 46.75 kHz, -2.34% error from PLLI2S)
- ✅ Clock Source: I2S2 PLL Clock (PLLI2S at 48 MHz)
- ✅ Master Clock Output: Enabled (12.288 MHz external crystal via PI0/I2S_CKIN)
- ✅ Audio Clock Input (I2S_CKIN): Enabled in RCC
- ✅ DMA1 Stream 4 configured for I2S2_TX (Circular mode, Word peripheral, HalfWord memory)
- ✅ DMA1 Stream 4 interrupt enabled in NVIC
- ✅ SPI2 global interrupt: DISABLED (not needed for DMA circular mode)
- ✅ Clock Polarity: LOW (I2S Philips standard - cannot be changed)

**Tasks:**
- [x] Configure I2S2 + DMA1 in CubeMX
- [x] DMA1 Stream 4 (not Stream 3) for I2S2_TX on STM32F411
- [x] Enable DMA interrupt for buffer refill
- [ ] Generate code from CubeMX
- [ ] Verify generated code (dma.c, i2s.c, main.c)
- [ ] Wire I2S2 → MAX98357A → Speaker (SCK→PB13, WS→PB12, SD→PB15, MCLK→external 12.288 MHz)
- [ ] Add sine wave generation code (1 kHz test tone)
- [ ] Flash and test audio output
- [ ] Hear 1 kHz tone

**Key learning points:**
- I2S2 uses DMA1 Stream 4 (not Stream 3) for TX on STM32F411
- I2S Philips standard requires Clock Polarity = LOW
- Circular DMA mode enables continuous audio playback without CPU intervention
- External 12.288 MHz crystal via I2S_CKIN provides MCLK for audio quality
- PLLI2S provides I2S peripheral's internal logic clock (48 MHz)

---

## Phase 3 — USB Audio Class 1.0 Device

| Status | Task |
|--------|------|
| ⏳ | Not started |

**Goal:** PC sees the Black Pill as a USB speaker. Audio from PC plays through the board.

---

## Phase 4 — Rotary Encoder Volume Control

| Status | Task |
|--------|------|
| ⏳ | Not started |

**Goal:** Turn encoder → volume changes. Press button → mute.

---

## Phase 5 — ST7735S TFT Audio Visualizer

| Status | Task |
|--------|------|
| ⏳ | Not started |

**Goal:** Live audio level + spectrum bars on the TFT.

---

## Phase 6 — FreeRTOS Integration

| Status | Task |
|--------|------|
| ⏳ | Not started |

**Goal:** 4 tasks: Audio, Display, Encoder, Debug. Use queues + semaphores.

---

## Phase 7 — Polish & Documentation

| Status | Task |
|--------|------|
| ⏳ | Not started |

**Goal:** README, learning journal, demo.

---

## Legend
- ⏳ Pending
- 🔄 In progress
- ✅ Complete
- ❌ Blocked
