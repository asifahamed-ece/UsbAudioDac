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
    │  USB Full-Speed (12 Mbps) — 44.1 kHz, 16-bit, mono
    ▼
STM32F411 Black Pill
    ├── USB OTG FS  ─── receives audio packets (44.1 kHz mono)
    ├── I2S2        ─── outputs PCM to MAX98357A (44.117 kHz)
    ├── DMA1        ─── moves PCM samples to I2S
    ├── SPI1 (PA5/PA7)      → drives ST7735S TFT display
    ├── TIM4 (encoder mode) ─── reads rotary encoder
    ├── DAC1        ─── PAM8403 hardware volume control
    └── USART2      ─── debug output
    │
    ├──▶ MAX98357A → Speaker
    ├──▶ ST7735S TFT (visualizer / volume)
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
**Goal:** Configure HSE → PLL → 48 MHz SYSCLK, PLLQ → 48 MHz USB, PLLI2S → 48 MHz I2S clock
**Peripherals:** RCC, PLL, PWR
**Time:** Day 2-4

**Tasks Completed:**
- [x] Read RM0383 Chapter 6 (RCC) — PLL configuration flow
- [x] Enable HSE (25 MHz external crystal on Black Pill, PH0/PH1)
- [x] Configure main PLL: PLLM=25, PLLN=384, PLLP=DIV8, PLLQ=8 → 48 MHz SYSCLK + 48 MHz USB
- [x] Configure PLLI2S: PLLI2SM=25, PLLI2SN=192, PLLI2SR=2 → 96 MHz I2SCLK
- [x] Verify clock config via CubeMX validation (no red warnings)
- [x] AHB Prescaler = 1 (HCLK = 48 MHz), APB1 Prescaler = 2, APB2 Prescaler = 1
- [x] FLASH_LATENCY_1 for 48 MHz @ 2.7-3.6 V
- [x] **Abandoned external I2S_CKIN plan:** PI0 is not exposed on the F411 CEU6 (UFQFPN48) package. PLLI2S at 96 MHz gives equivalent audio quality without extra hardware.
- [x] **Simplified from an earlier 60 MHz SYSCLK plan** (`M=15, N=144, P=4`) to 48 MHz so the whole system runs at a single frequency. Less to debug, same headroom for USB + I2S + later RTOS tasks.

**Final Configuration:**
```
HSE = 25.000 MHz (PH0/PH1 crystal)
SYSCLK = 48.000 MHz (PLL: M=25, N=384, P=DIV8)
USBCLK = 48.000 MHz (PLLQ=8)
I2SCLK = 96.000 MHz (PLLI2S: M=25, N=192, R=2)
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
- ✅ `HAL_I2S_MspInit` is the sole PLLI2S config site (PLLI2SN=192, PLLI2SM=25, PLLI2SR=2 → 96 MHz) — `SystemClock_Config` does NOT touch PLLI2S
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

### **Phase 3 — USB Audio Class 1.0 Device** ✅
**Goal:** PC recognizes Black Pill as a USB speaker, audio packets from PC play through MAX98357A
**Peripherals:** USB OTG FS (PA11/PA12), I2S2 + DMA1 (Phase 2)
**Time:** Day 7-12
**Mode:** TUTOR — I generated the descriptors + ST-library glue, you wrote the audio plumbing (ring buffer + I2S refill callbacks). Full debugging story is in `PROGRESS.md` → Phase 3 (three silent-failure bugs each took an evening to track down — read it before re-touching the USB code).

**Architecture:**
```
USB_PC ──USB──▶ usbd_audio.c ──▶ usbd_audio_if.c ──▶ ring_buffer.c ──▶ audio_i2s.c ──▶ MAX98357A
                (ST class        (user-side          (SPSC ring,         (DMA half/cplt
                 driver,          glue: AUDIO_         1024 samples        callbacks
                 isochronous      CMD_PLAY →          = 23 ms @ 44.1      refill from
                 OUT handler)     RingBuffer_Write)    kHz)                ring, silence
                                                                             on underrun)
```

**Audio format (advertised in USB descriptor):**
- **44.1 kHz, 16-bit, mono** (MAX98357A on our board is mono; I2S2 clocks at 44.117 kHz real)
- Implicit feedback — PC's clock is master
- Isochronous OUT endpoint 0x01, **88-byte packets every 1 ms** (44 mono int16 samples = 1 ms of audio)
- `AUDIO_OUT_PACKET = (USBD_AUDIO_FREQ * 2) / 1000 = 88`. Must agree with both the descriptor and the I2S clock or the ring overruns/underruns and you get silence.

**File breakdown — who wrote what:**

| File | Written by | Purpose |
|---|---|---|
| `USB_Audio_DAC_1.0/Middlewares/ST/STM32_USB_Device_Library/...` | ST vendor | Unmodified USB Audio class driver + OTG FS core. Provided by the library, not hand-written. |
| `USB_Audio_DAC_1.0/USB_DEVICE/App/usbd_audio_if.c` | Tutor | Bridges the class driver to our ring buffer. `AUDIO_AudioCmd_FS` is the user-side hook that the class driver calls with `AUDIO_CMD_PLAY` when a fresh USB packet arrives; this is where `RingBuffer_Write` lives. `TransferComplete_CallBack_FS` / `HalfTransfer_CallBack_FS` are the user-side hooks called by `USBD_AUDIO_Sync`. |
| `USB_Audio_DAC_1.0/USB_DEVICE/App/usbd_desc.c` | Tutor | Device descriptor + configuration descriptor; one mono streaming endpoint, bNrChannels=1. |
| `USB_Audio_DAC_1.0/USB_DEVICE/Target/usbd_conf.c` | Tutor | HAL_PCD_MspInit, USBD static-malloc hooks, **vbus_sensing = DISABLE** (Bug 1). |
| `USB_Audio_DAC_1.0/USB_DEVICE/Target/usbd_conf.h` | Tutor | **`#define USBD_AUDIO_FREQ 44100U`** — must match the .ioc or you get Bug 2. |
| `USB_Audio_DAC_1.0/Core/Src/ring_buffer.c` + `.h` | Student | Lock-free SPSC ring: `Reset`, `Write`, `Read`, `Available`, `Space`. 1024 int16, power of 2. |
| `USB_Audio_DAC_1.0/Core/Src/audio_i2s.c` + `.h` | Student | Pulls mono samples from the ring and lays them into the I2S DMA buffer as L=R stereo. `RefillHalfA` / `RefillHalfB` are the consumer side of the SPSC ring. Underrun = silence (memset to 0). |
| `USB_Audio_DAC_1.0/Core/Src/stm32f4xx_it.c` | Tutor | HAL I2S callbacks invoke `HalfTransfer_CallBack_FS` / `TransferComplete_CallBack_FS` **first** (Bug 3) and `AudioI2S_RefillHalfA/B` second. Order matters. |
| `USB_Audio_DAC_1.0/Core/Src/main.c` | Student | Init order: HAL → I2S2 + start circular DMA → USB device stack → main loop. |

**What we actually built (replaces the earlier "build order with verification" plan):**

| Step | What | Verifies with |
|---|---|---|
| 1 | `.ioc`: enable `USB_OTG_FS` as Device_Only, `USB_DEVICE` middleware → Audio Class 1.0, `USBD_AUDIO_FREQ=44100`, I2S2 at 44.1K with DMA1 Stream 4. Regenerate. | `make` succeeds; `Middlewares/.../usbd_audio.*` appears. |
| 2 | Vendor library: copy ST USB Device Library into `Middlewares/ST/STM32_USB_Device_Library/`. | `make` still succeeds. |
| 3 | Write `USB_DEVICE/App/usbd_audio_if.c`, `usbd_desc.c`, `Target/usbd_conf.c/.h`. | `make` succeeds; `lsusb` shows `bInterfaceClass=1 Audio` (after Bug 1 fix). |
| 4 | Write `Core/Src/ring_buffer.c` (SPSC, 1024 samples) and `Core/Src/audio_i2s.c` (pull from ring, lay into I2S DMA buffer as L=R). | `make` succeeds; ring logic testable in isolation. |
| 5 | Wire `AUDIO_AudioCmd_FS` (AUDIO_CMD_PLAY) → `RingBuffer_Write`. Wire `USBD_AUDIO_Sync` call from HAL I2S callbacks (Bug 3). | `speaker-test -D plughw:2,0 -c 1 -r 44100 -t sine -f 1000` plays 1 kHz out of speaker. **Phase 3 done.** |
| 6 | Verify with `aplay` on a real WAV. | Music plays. |

**Key descriptor bytes (so the next reader knows what to expect):**
- `bNrChannels = 1`, `bSubSlotSize = 2`, `bBitResolution = 16`, `tSamFreq = 0x00AC44` (44100)
- `wMaxPacketSize = 88`, `bInterval = 1`
- `bmAttributes = 0x00` (Sync) — PC is clock master
- `bSynchAddress = 0` — no explicit feedback endpoint

**What you'll have learned by the end of Phase 3:**
- USB descriptors (what each byte means)
- USB isochronous endpoints and implicit-feedback clocking
- VBUS sensing on OTG FS — and when to disable it
- The ST USB Audio library's split-API model (class driver → user via `AUDIO_*_FS` callbacks; user → class driver via `USBD_AUDIO_Sync`)
- Lock-free SPSC ring buffers (used in every audio device ever)
- The I2S half/complete callback pattern (used in every DMA audio pipeline)
- How PC and embedded negotiate audio format, and what happens when they disagree

**Acceptance test:**
- [x] `lsusb -v` shows device with bInterfaceClass=1 Audio
- [x] `aplay -l` (Linux) shows "USB Speaker"
- [x] `speaker-test -D plughw:2,0 -c 1 -r 44100 -t sine -f 1000` plays 1 kHz out of speaker
- [x] YouTube / `aplay` audio plays
- [x] Unplug → no PC error, replug → resumes
- [ ] 10-min stress test (no dropouts, ring stays bounded) — deferred, see PROGRESS.md "Deferred Items"

**Deliverable:** PC plays music through the Black Pill → speaker. Phase 3 complete.

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

### Build & Flash Commands

The CubeMX-generated project lives in `USB_Audio_DAC_1.0/`. Build and flash from that directory.

**Toolchain:** `arm-none-eabi-gcc` (Arch Repository, version 16.2.0)
**Flash tools:** `st-flash` (ST-Link V2) and `openocd` are available

```bash
cd USB_Audio_DAC_1.0
make                    # Full build
make V=1                # Verbose build
make clean              # Clean build artifacts
st-flash write build/USB_Audio_DAC_1.0.bin 0x08000000   # Flash
st-flash erase 0x08000000 512K                           # Erase
st-flash reset                                           # Reset
```

> The active project is the CubeMX-generated `USB_Audio_DAC_1.0/` (not a hand-rolled register-level project). It uses ST's HAL drivers and a CubeMX-generated Makefile.

---

## 8. Verification Checklist

- [x] Phase 0: LED blinks, "Hello World" prints on serial ✅
- [x] Phase 1: Clock tree configured (HSE=25 MHz, SYSCLK=48 MHz, USBCLK=48 MHz, I2SCLK=96 MHz from PLLI2S) ✅
- [x] Phase 2: **1 kHz tone verified on online frequency meter** ✅
- [x] Phase 3: **PC plays audio through MAX98357A** — `lsusb` shows Audio class, `speaker-test -f 1000/2000/3000/4000` all audible, `aplay` on WAV works. See Phase 3 section for the three-bug story. ✅
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
- Clock tree design (HSE → PLL → 48 MHz, PLLQ → 48 MHz USB, PLLI2S → 96 MHz I2S)
- Digital audio protocols (I2S, BCLK/LRCLK framing)
- DMA controller (circular buffers, double-buffering)
- USB 2.0 Full-Speed device (descriptors, endpoints, audio class)
- FreeRTOS (tasks, queues, semaphores, priority)
- I2C, SPI, USART, DAC, ADC, TIM peripherals
- Debugging embedded systems (serial output, buffer monitoring, underrun detection)

---

*Estimated total time: 3-4 weeks part-time (~2-3 hours per day)*
