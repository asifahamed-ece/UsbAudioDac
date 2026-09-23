# STM32F411 USB Audio DAC — 16-Band TFT Audio Visualizer Specification

## 1. Executive Summary

This specification defines the architecture, hardware interfacing, digital signal processing (DSP) pipeline, and graphics rendering engine for the real-time 16-band audio spectrum visualizer (Phase 5) on the STM32F411CEU6 Black Pill USB Audio Class 1.0 device.

The visualizer processes real-time 44.1 kHz PCM audio received over USB, performs a hardware-accelerated Fast Fourier Transform (FFT) via ARM CMSIS-DSP on the Cortex-M4 FPU, and renders a 16-band audio spectrum with floating peak-hold indicators and a Cyberpunk Cyan-Magenta neon color scheme onto a 128x128 ST7735S SPI TFT display at 50+ FPS.

---

## 2. Hardware Interface & Pin Configuration

### 2.1 Display Specifications
* **Controller:** Sitronix ST7735S
* **Display Type:** 1.44" SPI TFT LCD (RGB565, 16-bit color, 65,536 colors)
* **Resolution:** 128 × 128 pixels (Square format)
* **Orientation:** Standard Top-Down (MADCTL RGB color mode, coordinate offset: X=2, Y=3 / 0,0 configurable)

### 2.2 Pin Interconnect & Verification
All pin assignments have been verified against the STM32F411CEU6 datasheet, reference manual (RM0383), and existing audio/USB peripherals to ensure zero pin or DMA conflicts.

| Display Pin | STM32F411 Pin | Function / Peripheral | Mode | Notes |
|-------------|---------------|-----------------------|------|-------|
| **VCC** | 3V3 | Power Supply | Power | 3.3V logic & power |
| **GND** | GND | Ground Reference | Ground | Shared system ground |
| **SCL / SCK** | `PA5` | SPI1_SCK | AF5 (Alternate Function) | 12 MHz SPI Clock |
| **SDA / MOSI** | `PA7` | SPI1_MOSI | AF5 (Alternate Function) | Master Out Slave In |
| **CS** | `PB0` | GPIO Output (CS) | Push-Pull, High Speed | Active Low Chip Select |
| **DC / A0** | `PA0` | GPIO Output (DC) | Push-Pull, High Speed | 0 = Command, 1 = Data |
| **RESET** | `PA1` | GPIO Output (RST) | Push-Pull, High Speed | Active Low Hardware Reset |
| **LED / BL** | 3V3 | Backlight Power | Power | Direct 3.3V (keeps PA2 free for USART2) |

### 2.3 Peripheral Configuration
* **SPI1 Peripheral:**
  * Master Mode, Transmit Only (Full Duplex with MISO unused).
  * Data Size: 8-bit, MSB First.
  * Clock Polarity (CPOL) = Low (0), Clock Phase (CPHA) = 1 Edge (0) (SPI Mode 0).
  * APB2 Clock = 48 MHz $\rightarrow$ Prescaler = 4 $\rightarrow$ **SPI1 Baud Rate = 12.0 Mbit/s**.
  * Software Slave Management (NSS controlled via PB0 GPIO).

---

## 3. Audio DSP & Frequency Analysis Architecture

```
[I2S Refill Buffer] (44.1 kHz, 16-bit Mono)
        │
        ▼ (Tap 256 samples into Ping-Pong buffer)
[Hann Windowing Table] (float32_t w[n] = 0.5 * (1 - cos(2*pi*n / 255)))
        │
        ▼
[CMSIS-DSP arm_rfft_fast_f32] (256-point Real FFT, Hardware FPU)
        │
        ▼
[CMSIS-DSP arm_cmplx_mag_f32] (128 Frequency Bins, DC to 22.05 kHz)
        │
        ▼
[Logarithmic 16-Band Grouping] (~60 Hz to ~16 kHz)
        │
        ▼
[Log/dB Scaling & Attack/Decay Ballistics]
        │
        ▼
[Peak Hold Physics (Gravity Drop)]
        │
        ▼
[16 Bar Heights (0..90 px) + 16 Peak Dots]
```

### 3.1 Sample Acquisition
* **Sample Window Size ($N$):** 256 samples.
* **Sampling Rate ($F_s$):** 44,100 Hz.
* **Window Duration:** $256 / 44100 \approx 5.8\text{ ms}$.
* **Bin Resolution:** $\Delta f = F_s / N = 44100 / 256 \approx 172.26\text{ Hz}$.
* **Capture Hook:** During the I2S DMA refill callback (`AudioI2S_RefillHalfA` / `AudioI2S_RefillHalfB`), incoming int16 samples are written into a background acquisition buffer. When 256 samples are collected, a flag signals the main loop that a new FFT frame is ready.

### 3.2 Spectral Analysis Steps
1. **Windowing:** Multiply the 256 `int16_t` samples by a pre-computed Hann window stored in ROM to eliminate spectral leakage.
2. **RFFT Execution:** Call `arm_rfft_fast_f32(&rfft_instance, fft_in, fft_out, 0)` using Cortex-M4 hardware floating point. Execution time is $< 0.15\text{ ms}$.
3. **Magnitude Calculation:** Call `arm_cmplx_mag_f32(fft_out, fft_mag, 128)` to obtain the magnitude spectrum of the 128 positive frequency bins.
4. **Logarithmic 16-Band Mapping:**
   Group the 128 linear bins into 16 psychoacoustic frequency bands:
   * **Band 0 (60–180 Hz):** Sub-bass / Kick
   * **Band 1 (180–350 Hz):** Bass
   * **Band 2 (350–520 Hz):** Low Mids
   * **Band 3 (520–700 Hz):** Mids
   * **Band 4 (700–900 Hz):** Mids
   * **Band 5 (900–1.2 kHz):** Vocal body
   * **Band 6 (1.2–1.6 kHz):** High Mids
   * **Band 7 (1.6–2.2 kHz):** High Mids
   * **Band 8 (2.2–3.0 kHz):** Presence
   * **Band 9 (3.0–4.0 kHz):** Presence / Attack
   * **Band 10 (4.0–5.2 kHz):** Highs
   * **Band 11 (5.2–6.8 kHz):** Highs
   * **Band 12 (6.8–8.8 kHz):** Brilliance
   * **Band 13 (8.8–11.2 kHz):** Cymbals / Sibilance
   * **Band 14 (11.2–14.0 kHz):** Treble
   * **Band 15 (14.0–17.5 kHz):** Air

### 3.3 Dynamic Ballistics & Peak-Hold Physics
* **dB Logarithmic Compression:** Linear magnitudes are mapped to visual bar heights ($0 \dots 90\text{ pixels}$) using an efficient piecewise or log approximation:
  $$\text{TargetHeight} = \text{Clamp}\left(K \cdot \log_{10}(\text{Mag} + 1.0), 0, 90\right)$$
* **Attack / Decay Response:**
  * If $\text{TargetHeight} > \text{CurrentHeight}$: $\text{CurrentHeight} = \text{TargetHeight}$ (Instant attack on transients).
  * If $\text{TargetHeight} < \text{CurrentHeight}$: $\text{CurrentHeight} = \text{CurrentHeight} \times 0.78$ (Smooth exponential decay).
* **Peak-Hold Mechanics:**
  * Each band maintains a peak position `peak_y[16]` and a hold counter `peak_hold[16]`.
  * If bar height exceeds `peak_y`, `peak_y` updates to bar height and `peak_hold` is reset to 8 frames.
  * When `peak_hold` reaches 0, `peak_y` drops at 1 to 2 pixels per frame under gravity.

---

## 4. UI Layout & High-Performance Rendering

### 4.1 Screen Spatial Layout (128 × 128 Pixels)

```
0,0 ┌──────────────────────────────────────────────────────────┐ 127,0
    │ [USB AUDIO]                    44.1kHz  (●)  Y: 0..18    │  ← Status Header
    ├──────────────────────────────────────────────────────────┤
    │                                              Y: 19       │  ← Separator Line (Dark Slate)
    │   _       _           _       _       _                  │
    │  [ ]     [ ]         [ ]     [ ]     [ ]                 │  ← Peak Dots (White / Hot Pink)
    │  | |     | |         | |     | |     | |                 │
    │  |#|     | |   _     |#|     | |     |#|                 │  ← Electric Magenta (Top 30%)
    │  |#| _   |#|  [ ]    |#|     |#|     |#|                 │
    │  |#||#|  |#|  |#|    |#| _   |#| _   |#|                 │  ← Neon Cyan (Mid 40%)
    │  |#||#|  |#|  |#||#| |#||#|  |#||#|  |#|                 │
    │  |#||#|  |#|  |#||#| |#||#|  |#||#|  |#|                 │  ← Deep Indigo (Base 30%)
    ├──────────────────────────────────────────────────────────┤
    │  60Hz                    1kHz               16kHz Y: 120 │  ← Frequency Legend / Baseline
127 └──────────────────────────────────────────────────────────┘ 127,127
```

* **Header Area ($Y = 0 \dots 18$):**
  * Left text: `"USB AUDIO"` (6x8 font, Neon Cyan `0x07FF`).
  * Right text: `"44.1k"` (6x8 font, Slate Gray `0x7BEF`).
  * Stream Status: Live glowing dot indicating active USB audio streaming.
* **Separator ($Y = 19$):** 1px horizontal rule (`0x2104`).
* **Visualizer Window ($Y = 20 \dots 118$, Height $= 98\text{ px}$):**
  * 16 Bars: Width = **6 pixels**, Gap = **1 pixel**, Left/Right Margins = **8 pixels**.
  * Total visualizer width: $8 + (16 \times 6) + (15 \times 1) + 8 = 8 + 96 + 15 + 8 = 127\text{ px}$.
* **Baseline & Scale ($Y = 119 \dots 127$):**
  * 1px solid base rule at $Y = 119$.
  * Frequency tick marks at Bands 0 (60 Hz), 6 (1 kHz), and 15 (16 kHz).

### 4.2 Neon Cyberpunk Color Palette (RGB565)

| Element | RGB888 | RGB565 | Description |
|---|---|---|---|
| **Peak Hold Dot** | `#FFFFFF` / `#FF2A85` | `0xFFFF` / `0xF950` | Bright White or Hot Neon Pink |
| **Upper Bar (70% - 100%)** | `#FF007F` | `0xF80F` | Vivid Electric Magenta |
| **Mid Bar (30% - 70%)** | `#00F5FF` | `0x07BF` | Hyper Neon Cyan |
| **Lower Bar (0% - 30%)** | `#2B0066` | `0x280C` | Deep Synthwave Indigo / Purple |
| **Background** | `#000000` | `0x0000` | Pure Black |
| **Header Accent** | `#00E5FF` | `0x073E` | Bright Cyan Accent |

### 4.3 Differential Rendering Engine
To prevent display flicker without requiring 32 KB of RAM for a full framebuffer:
* For each band $i \in [0, 15]$:
  * Compare current rendered height $\text{h\_old}[i]$ with new calculated height $\text{h\_new}[i]$.
  * **If $\text{h\_new} > \text{h\_old}$:** Write new vertical block from $\text{h\_old}$ to $\text{h\_new}$ in neon gradient color.
  * **If $\text{h\_new} < \text{h\_old}$:** Clear the vacated block from $\text{h\_new}$ to $\text{h\_old}$ with black (`0x0000`).
  * **Peak Dot:** Clear old 1px dot position, draw new 1px dot.
* **SPI Bandwidth:** Transfers drop from 32,768 bytes/frame to $\approx 150 - 450\text{ bytes/frame}$, taking $< 0.5\text{ ms}$ over 12 MHz SPI. Frame rate comfortably achieves 50–60 FPS.

---

## 5. Software Architecture & File Breakdown

### 5.1 Modular File Structure
1. `USB_Audio_DAC_1.0/Core/Inc/st7735.h` & `Core/Src/st7735.c`:
   * Low-level SPI1 commands, GPIO toggling (`CS`, `DC`, `RST`).
   * ST7735S initialization routine and 128x128 window boundary logic.
   * Primitives: `ST7735_FillRect`, `ST7735_DrawPixel`, `ST7735_DrawChar`, `ST7735_DrawString`.
2. `USB_Audio_DAC_1.0/Core/Inc/audio_fft.h` & `Core/Src/audio_fft.c`:
   * CMSIS-DSP `arm_rfft_fast_instance_f32` setup.
   * Hann window array, 16-band log bin tables.
   * `AudioFFT_PutSamples(int16_t *samples, uint16_t count)` and `AudioFFT_Process(uint8_t out_bands[16])`.
3. `USB_Audio_DAC_1.0/Core/Inc/visualizer.h` & `Core/Src/visualizer.c`:
   * Visualizer layout constants, neon color LUTs.
   * Peak-hold physics state machine.
   * `Visualizer_Init()` and `Visualizer_Update()`.
4. `USB_Audio_DAC_1.0/Core/Src/audio_i2s.c`:
   * Taps audio stream during `AudioI2S_RefillHalfA` / `AudioI2S_RefillHalfB` into `AudioFFT_PutSamples`.
5. `USB_Audio_DAC_1.0/Core/Src/main.c`:
   * Peripheral inits: SPI1, GPIOs.
   * Main loop: 50 Hz non-blocking timer tick calling `Visualizer_Update()` and `HAL_IWDG_Refresh()`.
6. `USB_Audio_DAC_1.0/Makefile`:
   * Includes CMSIS-DSP source modules (`TransformFunctions`, `ComplexMathFunctions`, `CommonTables`).
   * Adds new C source files and include directories.

---

## 6. Execution & Verification Plan

### 6.1 Bring-Up & Verification Phases
1. **Phase 5.1: ST7735 Display Driver Bring-Up**
   * Initialize SPI1 and GPIO control lines.
   * Display color test patterns (Red, Green, Blue, Cyan, Magenta, White) and text string.
   * Confirm 128x128 screen alignment and orientation.
2. **Phase 5.2: DSP Frequency Pipeline Verification**
   * Play standard 1 kHz sine tone via PC host: `speaker-test -D plughw:... -c 1 -r 44100 -t sine -f 1000`.
   * Verify on visualizer that Band 5 (~1 kHz) lights up to maximum while neighboring bands show sharp roll-off.
3. **Phase 5.3: Frequency Sweep Verification**
   * Play a log chirp / sweep from 50 Hz to 15 kHz.
   * Confirm spectrum bars light up sequentially from Band 0 to Band 15.
4. **Phase 5.4: Full Audio & Dynamic Ballistics Stress Test**
   * Play complex music playback.
   * Confirm instant transient response on drums/percussion, smooth decay falloff, responsive floating peak-hold dots, and continuous glitch-free audio output.
