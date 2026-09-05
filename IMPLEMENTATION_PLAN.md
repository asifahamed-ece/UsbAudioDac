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
**Goal:** Configure HSE → PLL → 60 MHz SYSCLK, PLLQ → 48 MHz USB, PLLI2S → 48 MHz I2S clock
**Peripherals:** RCC, PLL, PWR
**Time:** Day 2-4

**Tasks Completed:**
- [x] Read RM0383 Chapter 6 (RCC) — PLL configuration flow
- [x] Enable HSE (25 MHz external crystal on Black Pill, PH0/PH1)
- [x] Configure main PLL: PLLM=15, PLLN=144, PLLP=4, PLLQ=5 → 60 MHz SYSCLK + 48 MHz USB
- [x] Configure PLLI2S: PLLI2SM=25, PLLI2SN=192, PLLI2SR=4 → 48 MHz I2SCLK
- [x] Verify clock config via CubeMX validation (no red warnings)
- [x] AHB Prescaler = 1 (HCLK = 60 MHz), APB1 Prescaler = 1, APB2 Prescaler = 1
- [x] FLASH_LATENCY_1 for 60 MHz @ 2.7-3.6 V
- [x] **Abandoned external I2S_CKIN plan:** PI0 is not exposed on the F411 CEU6 (UFQFPN48) package. PLLI2S at 48 MHz gives equivalent audio quality without extra hardware.

**Final Configuration:**
```
HSE = 25.000 MHz (PH0/PH1 crystal)
SYSCLK = 60.000 MHz (PLL: M=15, N=144, P=4)
USBCLK = 48.000 MHz (PLLQ=5)
I2SCLK = 48.000 MHz (PLLI2S: M=25, N=192, R=4)
CSS Enabled for clock fault detection
FLASH_LATENCY_1
```

**Deliverable:** Clock tree configured with exact frequencies. System ready for Phase 2.

---

### **Phase 2 — I2S + DMA Audio Output (1 kHz Tone)** ✅
**Goal:** Generate a 1 kHz sine wave and hear it on the speaker
**Peripherals:** I2S2, DMA1, GPIO (alt function 5)
**Time:** Day 4-7

**Final working configuration (CubeMX `.ioc` + `main.c`):**
- ✅ I2S2: Master TX, Philips standard, **16-bit data, 32-bit frame**, MCLK disabled
- ✅ Audio Frequency: `I2S_AUDIOFREQ_44K` (real ≈ 44.117 kHz, +0.04% error)
- ✅ Clock Source: `I2S_CLOCK_PLL` (PLLI2S=48 MHz)
- ✅ CPOL = LOW (Philips standard)
- ✅ DMA1 Stream 4 / Channel 0 for I2S2_TX, Circular mode, FIFO FULL, HALFWORD, priority HIGH
- ✅ DMA1_Stream4 IRQ enabled at priority 0,0
- ✅ `HAL_I2S_MspInit` does NOT re-init PLLI2S (kept the `SystemClock_Config` value)
- ✅ Correct pin map: **PB10=CK, PB12=WS, PB15=SD** (AF5). Earlier plan had SCK→PB13 — wrong; PB13 is not I2S2 CK on F411.
- ✅ 882-int16 buffer (441 stereo frames = 10 ms = exactly 10 cycles of 1 kHz)
- ✅ Sine generation emits L,R pairs (stereo frames), mono on MAX98357A

**Tasks:**
- [x] Configure I2S2 + DMA1 in CubeMX
- [x] Generate code from CubeMX → `USB_Audio_DAC_1.0/`
- [x] Wire I2S2 → MAX98357A → Speaker (PB10/PB12/PB15)
- [x] Add 1 kHz sine generation in main.c
- [x] Flash via `st-flash write build/USB_Audio_DAC_1.0.bin 0x08000000`
- [x] Verify audio with online frequency meter → **1000 Hz confirmed** ✅

**Key learning points — the "344 Hz then 100 Hz" debugging story:**

1. First build with 256-int16 buffer + 1 kHz tone → output measured 344 Hz.
2. Root cause: I2S pairs int16 into L+R frames. 256 int16 = 128 frames = 2.9 ms of audio. 2.9 ms × 1 kHz = 2.9 cycles, then DMA loops back to sample 0 → phase snap → loop period 2.9 ms = 344 Hz.
3. Fix: size buffer to contain an integer number of cycles. 44100 / 1000 = 44.1 frames per cycle, smallest integer multiple = 441 frames = 882 int16 = 10 ms = 10 cycles. Output: clean 1 kHz. ✅
4. Trying f=440 Hz and f=880 Hz with the same 882 buffer: 10 ms = 4.4 cycles / 8.8 cycles → 100 Hz buzz. **General rule:** `(buffer_frames / Fs) × f` must be an integer; smallest valid buffer for any `f` is `44100 / gcd(f, 44100)` frames.
5. Why music won't have this problem: real audio is streamed continuously into the DMA buffer (USB ISR fills the half just played). The buffer is never looped → no snap. The "integer-cycles" rule is a test-tone-only constraint.

**I2S divider math (for future debugging):**
- HAL formula: `i2sdiv = ROUND(i2sclk / (packetlength × AudioFreq))`
- For Philips 16-bit data: packetlength = 32 (16 bits × 2 channels), not 16
- With I2SCLK=48 MHz, AudioFreq=44.1 kHz: `i2sdiv = ROUND(48e6 / (32 × 44100)) = 17` → BCLK = 48e6/17/2 = 1.412 MHz
- If you ever see BCLK = 3 MHz instead of 1.4 MHz, the I2S peripheral is treating the frame as 64 bits (16B_EXTENDED), not 32 — check `I2S_Init.DataFormat`

**Deliverable:** Audible, verified 1 kHz tone on speaker. Phase 2 complete.

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
- [x] Phase 1: Clock tree configured (HSE=25MHz, SYSCLK=60MHz, USBCLK=48MHz, I2SCLK=48MHz from PLLI2S) ✅
- [x] Phase 2: **1 kHz tone verified on online frequency meter** ✅
- [ ] Phase 3: PC shows "USB Audio Device" in Sound settings, music plays
- [ ] Phase 4: Encoder rotates → volume changes, press → mute
- [ ] Phase 5: TFT shows live volume and audio level
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
- Clock tree design (HSE → PLL → 60 MHz, PLLQ → 48 MHz USB, PLLI2S → 48 MHz I2S)
- Digital audio protocols (I2S, BCLK/LRCLK framing)
- DMA controller (circular buffers, double-buffering)
- USB 2.0 Full-Speed device (descriptors, endpoints, audio class)
- FreeRTOS (tasks, queues, semaphores, priority)
- I2C, SPI, USART, DAC, ADC, TIM peripherals
- Debugging embedded systems (serial output, buffer monitoring, underrun detection)

---

*Estimated total time: 3-4 weeks part-time (~2-3 hours per day)*
