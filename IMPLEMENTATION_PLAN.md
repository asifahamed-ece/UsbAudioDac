# STM32F411 USB Audio Player — Implementation Plan

---

## Project Title

**"STM32F411 USB Audio Player with Real-Time Visualizer"**

## Project Idea (One-liner)

A self-built USB speaker: receive digital audio from a PC over USB, decode it to analog, amplify it, and play it through a speaker — with a live audio visualizer on an OLED display.

---

## 1. Project Overview

The STM32F411CEU6 Black Pill enumerates as a USB Audio Class 1.0 device. When connected to a PC, it appears as a standard USB speaker. Audio data received over USB is played through the MAX98357A I2S DAC + Class D amplifier, driving a small 4Ω mono speaker. A rotary encoder controls volume, and an SSD1306 OLED displays audio level / track information.

**Target board:** STM32F411CEU6 Black Pill (Cortex-M4, 100 MHz, 128 KB RAM, single-precision FPU)
**Target firmware:** FreeRTOS-based, with HAL peripherals and register-level clock configuration

---

## 2. Hardware Bill of Materials

| Item | Source | Cost |
|------|--------|------|
| STM32F411CEU6 Black Pill | Already have | $0 |
| MAX98357A I2S DAC + amp module | Already have | $0 |
| PAM8403 amp module | Already have | $0 |
| 4Ω small speaker (mono) | Already have | $0 |
| SSD1306 0.96" OLED (I2C) | Need to buy | ~$2 |
| Rotary encoder with push button | Need to buy | ~$1 |
| ST-Link V2 programmer | Need to buy | ~$2-3 |
| USB-C cable | Already have or buy | ~$1 |
| Jumper wires, perfboard | Already have or buy | ~$1-2 |

**Total extra cost:** ~$5-7

---

## 3. System Architecture

```
PC (music source)
    │
    │  USB Full-Speed (12 Mbps)
    ▼
STM32F411 Black Pill
    ├── USB OTG FS  ─── receives audio packets
    ├── I2S2        ─── outputs PCM to MAX98357A
    ├── DMA1        ─── moves PCM samples to I2S
    ├── I2C1        ─── drives SSD1306 OLED
    ├── TIM4 (encoder mode) ─── reads rotary encoder
    ├── DAC1        ─── PAM8403 hardware volume control
    └── USART2      ─── debug output
    │
    ├──▶ MAX98357A → Speaker
    ├──▶ SSD1306 OLED (visualizer / volume)
    └──▶ Rotary Encoder (volume + mute)
```

---

## 4. Implementation Phases

The firmware is built in **8 phases**, each with a clean, demonstrable deliverable. Each phase teaches a set of STM32 concepts and produces something you can show working.

---

### **Phase 0 — Toolchain Setup & Board Bring-Up** ✅
**Goal:** Install IDE, flash an LED, verify serial output
**Peripherals:** GPIO, USART2
**Time:** Day 1-2

**Tasks:**
- [ ] Install STM32CubeIDE
- [ ] Connect ST-Link V2 to Black Pill (SWCLK, SWDIO, GND, 3V3)
- [ ] Create new project for STM32F411CEU6
- [ ] Configure PA0 as GPIO output → blinky
- [ ] Configure USART2 (PA2 TX, PA3 RX) at 115200 baud
- [ ] Print "Hello World" over serial
- [ ] Test serial output with TeraTerm/PuTTY

**Deliverable:** LED blinks, serial prints "Hello World"

---

### **Phase 1 — Clock Tree from Scratch** ✅
**Goal:** Configure HSE → PLL → 60 MHz SYSCLK, PLLQ → 48 MHz USB, external 12.288 MHz crystal for I2S MCLK
**Peripherals:** RCC, PLL, PWR
**Time:** Day 2-4

**Tasks Completed:**
- [x] Read RM0383 Chapter 6 (RCC) — focus on PLL configuration flow
- [x] Enable HSE (25 MHz external crystal on Black Pill)
- [x] Configure PLL: PLLM=15, PLLN=144, PLLP=4, PLLQ=5 → 60 MHz SYSCLK, 48 MHz USB clock
- [x] Verify clock config via CubeMX validation (no red warnings)
- [x] External 12.288 MHz crystal connected to I2S_CKIN pin (PI0) for I2S audio clock
- [x] AHB Prescaler = 1 (HCLK = 60 MHz), APB1 Prescaler = 2 (PCLK1 = 30 MHz), APB2 Prescaler = 1 (PCLK2 = 60 MHz)
- [x] Note: PLLI2S not needed - using external I2S_CKIN for audio clock

**Final Configuration:**
```
HSE = 25.000 MHz (external crystal)
SYSCLK = 60.000 MHz (PLL: M=15, N=144, P=4)
USBCLK = 48.000 MHz (PLLQ=5)
I2S MCLK = 12.288 MHz (external crystal via I2S_CKIN pin)
CSS Enabled for clock fault detection
```

**Deliverable:** Clock tree configured with exact frequencies. System ready for Phase 2 (I2S + DMA).

---

### **Phase 2 — I2S + DMA Audio Output (1 kHz Tone)** 🔄
**Goal:** Generate a 1 kHz sine wave and hear it on the speaker
**Peripherals:** I2S2, DMA1, GPIO (alt function)
**Time:** Day 4-7

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

**Deliverable:** Audible 1 kHz tone from speaker using external 12.288 MHz I2S clock

---

### **Phase 3 — USB Audio Class 1.0 Device**
**Goal:** PC recognizes Black Pill as a USB speaker, music plays
**Peripherals:** USB OTG FS, NVIC, GPIO (PA11/PA12)
**Time:** Day 7-12

**Tasks:**
- [ ] Read RM0383 Chapter 31 (USB OTG FS)
- [ ] Enable USB_OTG_FS as Device in CubeMX
- [ ] Add USB Device middleware: Audio Class 1.0
- [ ] Configure USB clock: 48 MHz required (from PLLQ=5 — already configured in Phase 1)
- [ ] Customize audio descriptors: 48 kHz, 16-bit, mono
- [ ] Implement USB audio receive callback: copy incoming packets to I2S DMA buffer
- [ ] Add double-buffered ring buffer between USB and I2S
- [ ] Handle underrun (silence) gracefully
- [ ] Plug into PC: check "USB Audio Device" appears in Sound settings
- [ ] Play music, hear it on speaker

**Deliverable:** PC plays music through the Black Pill → speaker

---

### **Phase 4 — Rotary Encoder Volume Control**
**Goal:** Turn encoder to change volume, press to mute
**Peripherals:** TIM4 (encoder mode), EXTI, DAC1
**Time:** Day 12-14

**Tasks:**
- [ ] Read RM0383 Chapter 14 (TIM encoder mode)
- [ ] Wire rotary encoder: A pin → PB6, B pin → PB7, button → PB8
- [ ] Configure TIM4 in encoder mode 3 (count both edges on both channels)
- [ ] On encoder count change: update volume 0-100%
- [ ] Apply software gain: multiply PCM samples by (volume/100) before I2S
- [ ] Configure DAC1 on PA4 → RC filter (1 kΩ + 100 nF) → PAM8403 VOL pin
- [ ] Update DAC output to track software volume
- [ ] EXTI on button press → toggle mute

**Deliverable:** Turn encoder → music volume changes; press → mute

---

### **Phase 5 — ST7735S TFT Audio Visualizer**
**Goal:** Display volume level and audio spectrum bars in full color
**Peripherals:** SPI1, GPIO
**Time:** Day 14-17

**Tasks:**
- [ ] Wire ST7735S TFT: SCK → PA5, MOSI → PA7, CS → PB0, DC → PA0, RST → PA1, LED → PA2
**Note:** Updated to use ST7735S TFT (SPI interface) instead of SSD1306 OLED (I2C) for full-color visualizer.
- [ ] Find/port a minimal ST7735S SPI driver (don't write from scratch — use Adafruit_ST7735 or TFT_eSPI)
- [ ] Configure SPI1 in master TX mode, clock ≤ 18 MHz, MSB first, mode 0
- [ ] Display: current volume %, audio level bar, status text
- [ ] Calculate audio level: peak of last N PCM samples
- [ ] Display audio spectrum (FFT) with colored bars (green→yellow→red gradient)
- [ ] Update display at 30-60 Hz (smooth animation, no flicker)
- [ ] Add startup splash screen with project name in color
- [ ] Implement basic drawing primitives: rectangles, lines, text, color fills

**Deliverable:** ST7735S shows live volume, audio level, and spectrum analyzer

**Note:** CS pin moved from PA4 to PB0 to avoid conflict with DAC1 in Phase 4.

---

### **Phase 6 — FreeRTOS Integration**
**Goal:** Migrate to FreeRTOS with distinct tasks
**Peripherals:** SysTick, NVIC priorities
**Time:** Day 17-22

**Tasks:**
- [ ] Add FreeRTOS via CubeMX (CMSIS-RTOS wrapper or native FreeRTOS)
- [ ] Configure heap (heap_4.c) and 4 tasks:
  - **Task Audio** (priority HIGH): USB → ring buffer → I2S DMA refill
  - **Task Display** (priority MED): OLED update at 10 Hz
  - **Task Encoder** (priority MED): poll encoder, update volume
  - **Task Debug** (priority LOW): USART stats every 1 sec
- [ ] Use queue: Audio task → Display task (audio level samples)
- [ ] Use binary semaphore: encoder interrupt → Display task (volume changed)
- [ ] Configure USB interrupt priority to be above configMAX_SYSCALL_INTERRUPT_PRIORITY
- [ ] Monitor task stack usage with uxTaskGetStackHighWaterMark()
- [ ] Stress test: 30+ minutes of playback without glitches

**Deliverable:** Audio plays cleanly with all 4 tasks running

---

### **Phase 7 — Polish & Documentation**
**Goal:** Clean code, write README, prepare for project demo
**Time:** Day 22-25

**Tasks:**
- [ ] Add boot splash screen with project title
- [ ] Verify all 7 phases work end-to-end
- [ ] Code cleanup: comments, naming, file structure
- [ ] Write README.md: build instructions, hardware wiring diagram, usage
- [ ] Write learning journal: what each phase taught
- [ ] Optional: 3D print a small enclosure

**Deliverable:** Complete project ready for demo

---

## 5. Peripherals Coverage Map

| Peripheral | RM Chapter | Used In |
|------------|-----------|---------|
| RCC, PLL, PLLI2S | Ch. 6 | Phase 1 |
| PWR | Ch. 5 | Phase 1 |
| GPIO | Ch. 8 | All phases |
| DMA1 | Ch. 9 | Phase 2, 3, 6 |
| NVIC, EXTI | Ch. 10 | Phase 4, 6 |
| ADC1 | Ch. 11 | (optional, not in core) |
| DAC1 | Ch. 12 | Phase 4 |
| TIM4 (encoder) | Ch. 14 | Phase 4 |
| TIM3 (periodic) | Ch. 13 | Phase 6 |
| I2C1 | Ch. 27 | Phase 5 |
| I2S2 (via SPI2) | Ch. 28 | Phase 2, 3, 6 |
| USART2 | Ch. 26 | Phase 0, 3, 6 |
| USB OTG FS | Ch. 31 | Phase 3, 6 |
| SysTick | Core | Phase 6 |
| FPU | Core | (optional, CMSIS-DSP) |

---

## 6. Key Files to Write

```
Core/
├── Src/
│   ├── main.c                  — system init, task creation
│   ├── clocks.c/h              — Phase 1: register-level clock config
│   ├── audio_i2s.c/h           — Phase 2-3: I2S + DMA driver
│   ├── usb_audio.c/h           — Phase 3: USB audio callbacks
│   ├── encoder.c/h             — Phase 4: rotary encoder driver
│   ├── volume.c/h              — Phase 4: software + hardware volume
│   ├── display.c/h             — Phase 5: SSD1306 + visualizer
│   └── ring_buffer.c/h         — Phase 3: USB ↔ I2S buffer
├── Inc/
│   └── (headers)
└── Startup/
    └── startup_stm32f411ceux.s
```

---

## 7. Tools Required

| Tool | Purpose | Cost |
|------|---------|------|
| STM32CubeIDE | IDE + compiler | Free |
| STM32CubeMX | Peripheral config | Free |
| ST-Link V2 | Flash + debug | ~$2-3 |
| USB-Serial adapter (CP2102/CH340) | Optional, for debug serial | ~$1 |
| TeraTerm / PuTTY | Serial terminal | Free |
| PC with music player | Audio source | Free |
| Multimeter | Voltage checks | Already have |

**No logic analyzer or oscilloscope required** — clock and signal verification done via serial debug output and audio listening tests.

---

## 8. Verification Checklist

- [x] Phase 0: LED blinks, "Hello World" prints on serial ✅
- [x] Phase 1: Clock tree configured (HSE=25MHz, SYSCLK=60MHz, USBCLK=48MHz, I2SCLK=12.288MHz external) ✅
- [ ] Phase 2: 1 kHz tone audible from speaker (in progress - DMA1 Stream 4 configured)
- [ ] Phase 3: PC shows "USB Audio Device" in Sound settings, music plays
- [ ] Phase 4: Encoder rotates → volume changes, press → mute
- [ ] Phase 5: OLED shows live volume and audio level
- [ ] Phase 6: 30-min stress test passes with all RTOS tasks running
- [ ] Phase 7: README complete, code documented

---

## 9. Stretch Goals (After Core Project)

- [ ] **Stereo audio:** Add second MAX98357A, configure I2S for stereo
- [ ] **Audio effects:** Add CMSIS-DSP for real-time EQ or simple delay/reverb
- [ ] **SD card logging:** Log audio statistics to SD card
- [ ] **Custom PCB:** Design full circuit in KiCad
- [ ] **Port to STM32H743:** Move firmware to H7, use M7 DSP instructions
- [ ] **TOSLINK output:** Add optical digital output

---

## 10. Learning Outcomes

By completion, you will have hands-on experience with:
- ARM Cortex-M4 architecture (NVIC, FPU, SysTick)
- Clock tree design (HSE → PLL → 60 MHz, PLLQ → 48 MHz USB, external 12.288 MHz I2S)
- Digital audio protocols (I2S, BCLK/LRCLK framing)
- DMA controller (circular buffers, double-buffering)
- USB 2.0 Full-Speed device (descriptors, endpoints, audio class)
- FreeRTOS (tasks, queues, semaphores, priority)
- I2C, SPI, USART, DAC, ADC, TIM peripherals
- Debugging embedded systems (serial output, buffer monitoring, underrun detection)

---

*Estimated total time: 3-4 weeks part-time (~2-3 hours per day)*
