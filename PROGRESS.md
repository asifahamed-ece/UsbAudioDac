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

## Phase 1 — Clock Tree (HSE → PLL → 60 MHz SYSCLK, PLLQ → 48 MHz USB, PLLI2S → 48 MHz I2S)

| Status | Task |
|--------|------|
| ✅ | **Completed** |

**Goal:** Achieve the clock configuration needed for USB audio + I2S output.

**What we have (final working configuration):**
```
HSE = 25.000 MHz (external crystal on PH0/PH1)
SYSCLK = 60.000 MHz (PLL: M=15, N=144, P=4)
USBCLK = 48.000 MHz (PLLQ=5)
I2SCLK = 48.000 MHz (PLLI2S: M=25, N=192, R=4) → drives I2S2 peripheral
CSS Enabled for clock fault detection
AHB=1, APB1=1, APB2=1 (all peripherals at full speed)
FLASH_LATENCY_1
```

**Why this configuration works:**
- **USB audio:** 48 MHz USB clock meets ±0.25% accuracy for Full-Speed enumeration
- **I2S audio:** PLLI2S at 48 MHz drives I2S2's internal prescaler; together with a Philips-standard 16-bit frame the HAL divider yields the requested 44.1 kHz LRCLK (real ≈ 44.117 kHz, +0.04% error)
- **System processing:** 60 MHz CPU is enough headroom for USB packets, audio buffering, and later RTOS tasks
- **No external I2S_CKIN:** early plan assumed an external 12.288 MHz crystal on PI0. Dropped because (a) PI0 is not brought out on the Black Pill header, and (b) PLLI2S at 48 MHz gives equivalent audio quality for our use case
- **Clock separation:** SYSCLK and I2SCLK are derived from independent PLLs, eliminating beat frequencies

**Clock tree math:**
- Main PLL: `VCO = (HSE/PLLM) × PLLN = (25/15) × 144 = 240 MHz`
- `SYSCLK = VCO / PLLP = 240/4 = 60 MHz`
- `USBCLK = VCO / PLLQ = 240/5 = 48 MHz`
- PLLI2S: `VCO_I2S = (HSE/PLLI2SM) × PLLI2SN = (25/25) × 192 = 192 MHz`
- `I2SCLK = VCO_I2S / PLLI2SR = 192/4 = 48 MHz`

**Note on earlier external-MCLK plan:** the F411 CEU6 package (UFQFPN48) does not expose PI0/I2S_CKIN, so the external 12.288 MHz crystal approach was abandoned. PLLI2S at 48 MHz gives the audio quality we need without the extra hardware.

**Key achievements:**
- Moved from HSI 16 MHz to HSE 25 MHz crystal
- Configured main PLL to 60 MHz SYSCLK, 48 MHz USB
- Configured PLLI2S to 48 MHz for I2S2
- All clocks validated via CubeMX clock tree (no red warnings)

**Deliverable:** Clock tree configured. System ready for Phase 2.

---

## Phase 2 — I2S + DMA Audio Output (1 kHz test tone) ✅

| Status | Task |
|--------|------|
| ✅ | **Completed — 1 kHz tone verified with online frequency meter** |

**Goal:** Generate a 1 kHz sine wave in code, push it to MAX98357A over I2S2 + DMA1, hear it on the speaker.

**Final working configuration (CubeMX `.ioc` + `main.c`):**
- ✅ I2S2: Master TX, Philips standard, 16-bit data, MCLK disabled, CPOL = LOW
- ✅ I2S2 Audio Frequency: I2S_AUDIOFREQ_44K (real ≈ 44.117 kHz from PLLI2S=48 MHz)
- ✅ I2S2 Clock Source: I2S_CLOCK_PLL (PLLI2S at 48 MHz)
- ✅ DMA1 Stream 4 for I2S2_TX (DMA channel 0), Circular mode, FIFO enabled, HALFWORD both sides
- ✅ DMA1_Stream4 IRQ priority 0,0 (NVIC enabled)
- ✅ HAL_I2S_MspInit does NOT touch PLLI2S (lets `SystemClock_Config` set it once)
- ✅ MAX98357A wired: BCLK←PB10, LRCK/WS←PB12, SD←PB15; SD pin has L/R channel-select strap (see BOM.md)

**Pin note:** `PB13` is not the I2S2 CK pin on the F411 — the alternate-function 5 mapping is **PB10=CK, PB12=WS, PB15=SD**. Earlier plan had `SCK→PB13`; that is wrong for I2S2 and was corrected.

**Tasks:**
- [x] Configure I2S2 + DMA1 in CubeMX
- [x] Verify pin map (PB10/PB12/PB15, not PB13)
- [x] Remove duplicate PLLI2S init from `HAL_I2S_MspInit` (was overwriting main.c's config)
- [x] Use `RCC_PERIPHCLK_I2S` (not `_APB1`/`_APB2` — those are F412/F413/F446 only)
- [x] Wire I2S2 → MAX98357A → Speaker
- [x] Generate 1 kHz sine in 882-int16 buffer
- [x] Flash and verify on online frequency meter → **1000 Hz confirmed** ✅

**The "344 Hz then 100 Hz buzz" debugging story — important lesson:**

1. First build with 256-int16 buffer + `Fs=44100`, `f=1000` → output measured 344 Hz.
2. Realised I2S pairs int16 samples into L+R stereo frames: **256 int16 = 128 frames = 2.9 ms of audio**. 2.9 ms × 1 kHz = only 2.9 cycles, then DMA loops back to sample 0 → phase snap → loop period 2.9 ms = **344 Hz**.
3. Fixed by sizing buffer to contain an integer number of cycles: `441 frames = 882 int16 = 10 ms = exactly 10 cycles of 1 kHz`. Now no snap, clean **1 kHz** output. ✅
4. Then changed `f=440` to verify: still heard 100 Hz buzz. Same root cause — buffer is 10 ms = 4.4 cycles of 440 Hz, snap → 100 Hz. **General rule:** `(buffer_frames / Fs) × f` must be an integer. For any `f`, the smallest valid buffer is `44100 / gcd(f, 44100)` frames.
5. Same effect at 880 Hz: 10 ms = 8.8 cycles → 100 Hz buzz.

**Why music won't have this problem:** real audio is streamed continuously into the DMA buffer via the USB ISR — the buffer is never looped, so no snap. The "integer-cycles" rule is a test-tone constraint only. (See IMPLEMENTATION_PLAN.md → Phase 3 for the ring-buffer design.)

**Key learning points:**
- I2S2 uses DMA1 Stream 4 / Channel 0 for TX on STM32F411
- I2S pins on F411 (AF5): **PB10=CK, PB12=WS, PB15=SD**
- I2S Philips standard + 16-bit data → frame is 32 bits; HAL divider formula `i2sdiv=(i2sclk/AudioFreq/packetlength)` must be matched
- Circular DMA mode replays the buffer forever; if you use one, the buffer must encode an integer number of output cycles
- `RCC_PERIPHCLK_I2S` is the correct define for F411; `_APB1`/`_APB2` are for F412/F413/F446
- Don't re-init PLLI2S inside `HAL_I2S_MspInit` if you already set it in `SystemClock_Config` — the HAL will overwrite your values

**Deliverable:** 1 kHz sine tone audible on speaker, verified with online frequency meter. Phase 2 complete.

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
