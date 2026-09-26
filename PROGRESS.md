# Project Progress

STM32F411 USB Audio Player — block-by-block learning log.

---

## Changelog

### [Unreleased]

**2026-09-26 — Exact 48 kHz sample rate (fixes the periodic zero-gap "tap")**

- **Root cause.** PLLI2S was `M=25, N=192, R=2` → I2SCLK 96 MHz, so `HAL_I2S_Init` chose
  `I2SDIV=34, ODD=0` → real Fs = **44 117.647 Hz** while USB declared **44 100 Hz**. That is
  **+400 ppm**: the I2S consumed 17.6 samples/s more than the host delivered, so the 2048-sample
  ring slowly drained and each DMA refill came up short. `AudioI2S_RefillHalfA/B` zeroes the
  unfilled tail, so every short refill became a **zero-gap at the buffer rate — 100.27 Hz
  (10 ms full buffer) / 200.53 Hz (5 ms half buffer)**. That is the "periodic thud/tap, several
  times a second" symptom. There was no way for the host to correct it: the streaming interface
  declares only the OUT endpoint (no feedback EP `0x81`) and `USBD_AUDIO_SOF` is a no-op stub,
  so the `Sof_enable = ENABLE` from the earlier fix was inert.
- **Why not 44.1 kHz.** An exhaustive search over every legal PLLI2S divisor
  (`M` 2–63, `N` 50–432, `R` 2–7, VCO input 0.95–2.10 MHz, `k = 2·I2SDIV+ODD` ∈ [4,511])
  found **zero** solutions for 44 100 Hz. `I2SCLK` would have to be an exact multiple of
  1 411 200 Hz, but from a 25 MHz HSE `I2SCLK = 25·N/(M·R)` is a ratio of small integers and can
  never land on that lattice. The same search found **three** exact solutions for 48 000 Hz.
- **Fix — switch the device to exactly 48 000 Hz:**
  - `Core/Src/stm32f4xx_hal_msp.c`: `PLLI2SN` 192 → **384** (VCO 384 MHz, I2SCLK 192 MHz).
    In spec per datasheet Table 42: `fVCO_OUT` 100–432 MHz, `fPLLI2S_OUT` max 216 MHz,
    `fPLLI2S_IN` 1 MHz.
  - `Core/Src/main.c`: `I2S_AUDIOFREQ_44K` → `I2S_AUDIOFREQ_48K` ⇒ HAL picks `I2SDIV=62, ODD=1`.
    **Fs = 192 MHz / (32 × 125) = 48 000.000000 Hz, drift 0.000 samples/s.**
  - `USB_DEVICE/Target/usbd_conf.h`: `USBD_AUDIO_FREQ` 44100U → **48000U**.
  - `usbd_audio.h`: `AUDIO_OUT_PACKET_MAX` 90U → **96U**. `AUDIO_OUT_PACKET` → 96 and
    `AUDIO_TOTAL_BUF_SIZE` → 7680 derive automatically.
  - `Core/Src/audio_i2s.c`: `GEN_SR` → 48000.0f so the `dbg_bypass_usb` isolation tone stays
    correct.
  - `.ioc`: `RCC.PLLI2SN=384`, `I2S2.AudioFreq`, `RealAudioFreq=48 KHz`, `ErrorAudioFreq=0.00 %`,
    `USBD_AUDIO_FREQ=48000`, plus derived `I2SClocksFreq_Value`/`VCOI2SOutputFreq_Value`/
    `VcooutputI2S` — so CubeMX regeneration cannot revert this. Also corrected
    `USB_OTG_FS.Sof_enable` in the `.ioc` (`DISABLE` → `ENABLE`); it disagreed with
    `usbd_conf.c` and would have silently reverted the earlier SOF fix on regeneration.
- **Side benefit.** At 48 kHz, USB FS `bInterval=1` gives exactly 48 samples = **96 bytes every
  millisecond, every frame** — the 44.1 kHz 88/90 "long frame" alternation (the original cause of
  the `wMaxPacketSize` mismatch) no longer exists, and `AUDIO_OUT_PACKET_MAX` is exact.
- **Verification.** `make clean && make` → 0 warnings; `make test` → 5227 + 31 checks, 0 failures.
  Descriptor decoded from the linked image: mono, 16-bit, **bSamFreq = 48000**,
  **wMaxPacketSize = 96**, `bInterval = 1`, `bNumEndpoints = 1`; no trace of 44100 or the old
  90-byte endpoint.
- **Result on hardware (2026-09-26, "Standard recording 6.mp3", 61.9 s): the periodic thuds are
  gone.** Analysis of the new recording found **zero dropouts, zero click events after the first
  250 ms, and no silence gaps** across 61.9 s of playback (the only silent span is the 89 ms
  before playback started). Peak measured 3rd harmonic also improved from ≈ −2 dB to a median of
  −18 dB relative to the fundamental.
- **Two symptoms remain, both attributable to the phone's recorder, not the firmware:**
  1. *Amplitude highs/lows.* The level is modulated at **0.162 Hz (6.2 s period)**, and the
     modulation is **identical in the Left and Right channels** — the signature of a recorder
     AGC. A DAC/USB fault would either be far faster (tied to the 4.58 ms refill) or would
     differ between the two mics.
  2. *3 kHz harmonics.* Harmonic ratios measure exactly 1 : 2.0000 : 2.9998 : 3.9998 : 5.0002,
     so it is genuine non-linear distortion, not intermodulation. But its level **switches
     between +10 dB and −45 dB relative to the fundamental on a 0.5–2 s timescale**, and the
     "clean" windows (e.g. 47.5–53.3 s, 3rd harmonic at −38…−46 dB) are entirely normal for a
     Class-D amp. Decisively, across 83 windows the distortion was in **both channels in 59 %**
     and in **Left only in 41 %, and in Right only in 0 %** — a single-sided pattern that a
     mono DAC→amp→speaker path cannot produce, since the firmware writes bit-identical samples
     to L and R (`audio_i2s_buffer[2i] = audio_i2s_buffer[2i+1]`). A 3rd harmonic *louder than
     the fundamental* is a hard square-wave clip, i.e. a limiter — not a Class-D amplifier at
     −25 dBFS.
  → **Next diagnostic (needs hardware):** `dbg_bypass_usb = 1` over SWD makes the MCU emit a
  mathematically pure 1 kHz sine through the identical DMA→I2S2→MAX98357A→speaker path. If the
  3rd harmonic and the level wander still appear in the recording, they are conclusively the
  phone. Also read `dbg_partial_count` / `dbg_min_read` for objective confirmation of this fix.
  → **Cheap control:** record a laptop or phone speaker playing the same file with the same phone
  at the same distance.
- **Known follow-ups (not in this change):** (a) `audio_fft.c`'s 12-band table is indexed in bins,
  so at 48 kHz `df` goes 43.07 → 46.875 Hz and the visualizer bands shift ~8.8 % in frequency;
  (b) `HAL_I2S_TxHalfCpltCallback` still runs `USBD_AUDIO_Sync` (up to 1760 samples, scalar copy)
  plus the refill at NVIC priority 0 every ~4.6 ms — a ~300 µs window with USB and SysTick
  masked.

**2026-09-23 — USB Audio stability, buffer overrun fix & TFT visualizer**

- **USB Audio Circular Buffer Overrun & Screeching Fix** (`usbd_audio.h`, `usbd_audio.c`):
  - Fixed root cause of screeching and device disconnect/reset during playback. `AUDIO_OUT_PACKET_MAX` was 90 bytes (for 44.1 kHz fractional frames) while the ring buffer was sized at `88 * 80 = 7040` bytes. The HAL received up to 90 bytes linearly into `&buffer[wr_ptr]`, spilling up to 16 bytes past array bounds into `USBD_AUDIO_HandleTypeDef` struct fields (`rd_ptr`, `wr_ptr`, `control`).
  - Added `AUDIO_OUT_PACKET_MAX` spill pad to `haudio->buffer` (`7040 + 90` bytes). Static allocation pool auto-scaled via `sizeof`.
  - Implemented true modulo wrap (`wr_ptr -= AUDIO_TOTAL_BUF_SIZE`) in `USBD_AUDIO_DataOut` instead of snap-to-zero, preventing sample loss on straddled frames.
  - Cleared EP0 control state (`cmd`, `len`, `unit`) unconditionally on all `SET_CUR` requests in `USBD_AUDIO_EP0_RxReady`.
  - Clamped `rd_ptr` in `USBD_AUDIO_Sync` to guarantee bounds safety.
- **STM32F411 Memory Architecture Correction**:
  - Removed invalid `.ccmram` section placements from `audio_fft.c`. STM32F411 has contiguous 128 KB SRAM at `0x20000000` (no CCMRAM at `0x10000000` like F405/F407). Attempted access to `0x10000000` caused an immediate BusFault / HardFault.
- **Build & Watchdog Enhancements**:
  - Added `make release` (`DEBUG=0 OPT=-O2`) target to `Makefile` with active IWDG, while keeping `DEBUG_NO_WATCHDOG` for debug sessions.
- **ST7735S TFT Spectrum Visualizer (Phase 5)**:
  - 16-band log-spaced FFT spectrum analyzer (CMSIS-DSP RFFT 256) running smoothly. Fixed ST7735 SPI DMA init sequence so USB enumeration is not blocked.

**2026-09-17 — Reliability & test infrastructure (Phase 3.5)**

- **Linker** (`USB_Audio_DAC_1.0/STM32F411xx_FLASH.ld`): declared the 64 KB CCMRAM region (0x10000000) with a `.ccmram` (NOLOAD) section for CPU-only data; bumped `_Min_Stack_Size` from 0x800 to 0x1000.
- **Tests** (`USB_Audio_DAC_1.0/tests/`): new host-simulated unit suite for the SPSC ring buffer — 8 tests / 5227 assertions, clean under ASan/UBSan. Run with `make -C tests run`. No ARM toolchain needed.
- **Watchdog**: enabled `HAL_IWDG_MODULE_ENABLED`, linked the IWDG HAL driver, started a windowless IWDG (~1 s) in `main.c`, refreshed each main-loop pass. A stuck main loop now ends in a reset instead of a dead device.
- **Build**: added `make flash`, `make test`, and `make size` convenience targets to the firmware Makefile.
- Deferred items resolved: `.ccmram` linker section, stack 0x800 → 0x1000.

### [0.x] — Phases 0-3 (historical)

- Phase 0: toolchain, board bring-up, LED blink.
- Phase 1: clock tree (HSE 25 MHz → 48 MHz SYSCLK/USB, PLLI2S → 96 MHz I2S).
- Phase 2: I2S2 + DMA, 1 kHz sine verified on speaker.
- Phase 3: USB Audio Class 1.0 device — PC streams 44.1 kHz/16-bit audio through the board to the MAX98357A. Three silent-failure bugs chased down and documented below.

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

## Phase 1 — Clock Tree (HSE → PLL → 48 MHz SYSCLK, PLLQ → 48 MHz USB, PLLI2S → 96 MHz I2S)

| Status | Task |
|--------|------|
| ✅ | **Completed** |

**Goal:** Achieve the clock configuration needed for USB audio + I2S output.

**What we have (final working configuration, in `.ioc`):**
```
HSE = 25.000 MHz (external crystal on PH0/PH1)
SYSCLK = 48.000 MHz (PLL: M=25, N=384, P=DIV8)
USBCLK = 48.000 MHz (PLLQ=8)
I2SCLK = 96.000 MHz (PLLI2S: M=25, N=192, R=2) → drives I2S2 peripheral
CSS Enabled for clock fault detection
AHB=1, APB1=DIV2, APB2=1
FLASH_LATENCY_1
```

**Why this configuration works:**
- **USB audio:** 48 MHz USB clock meets ±0.25% accuracy for Full-Speed enumeration
- **I2S audio:** PLLI2S at 96 MHz drives I2S2's internal prescaler; together with a Philips-standard 16-bit frame the HAL divider yields the requested 44.1 kHz LRCLK (real ≈ 44.117 kHz, +0.04% error)
- **System processing:** 48 MHz CPU is enough headroom for USB packets, audio buffering, and later RTOS tasks. The 60 MHz / 100 MHz designs considered earlier would work too, but 48 MHz keeps the bus dividers simple and the whole system at one frequency
- **No external I2S_CKIN:** early plan assumed an external 12.288 MHz crystal on PI0. Dropped because (a) PI0 is not brought out on the Black Pill header, and (b) PLLI2S at 96 MHz gives equivalent audio quality for our use case
- **Clock separation:** SYSCLK and I2SCLK are derived from independent PLLs, eliminating beat frequencies

**Clock tree math:**
- Main PLL: `VCO = (HSE/PLLM) × PLLN = (25/25) × 384 = 384 MHz`
- `SYSCLK = VCO / PLLP = 384/8 = 48 MHz`
- `USBCLK = VCO / PLLQ = 384/8 = 48 MHz`
- PLLI2S: `VCO_I2S = (HSE/PLLI2SM) × PLLI2SN = (25/25) × 192 = 192 MHz`
- `I2SCLK = VCO_I2S / PLLI2SR = 192/2 = 96 MHz`

**Note on earlier external-MCLK plan:** the F411 CEU6 package (UFQFPN48) does not expose PI0/I2S_CKIN, so the external 12.288 MHz crystal approach was abandoned. PLLI2S at 96 MHz gives the audio quality we need without the extra hardware.

**Note on earlier 60 MHz SYSCLK plan:** the design phase considered `M=15, N=144, P=4` to get a 60 MHz SYSCLK with the same 48 MHz USB clock. This was simplified to 48 MHz SYSCLK so that AHB and APB1/APB2 all run at the same frequency — fewer dividers, less to debug. The .ioc final config (M=25, N=384, P=DIV8) is what's actually on the board.

**Key achievements:**
- Moved from HSI 16 MHz to HSE 25 MHz crystal
- Configured main PLL to 48 MHz SYSCLK, 48 MHz USB
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
- ✅ I2S2 Audio Frequency: I2S_AUDIOFREQ_44K (real ≈ 44.117 kHz from PLLI2S=96 MHz)
- ✅ I2S2 Clock Source: I2S_CLOCK_PLL (PLLI2S at 96 MHz)
- ✅ DMA1 Stream 4 for I2S2_TX (DMA channel 0), Circular mode, FIFO enabled, HALFWORD both sides
- ✅ DMA1_Stream4 IRQ priority 0,0 (NVIC enabled)
- ✅ HAL_I2S_MspInit is the sole PLLI2S config site (PLLI2SN=192, PLLI2SM=25, PLLI2SR=2) — SystemClock_Config does NOT touch PLLI2S
- ✅ MAX98357A wired: BCLK←PB10, LRCK/WS←PB12, SD←PB15; SD pin has L/R channel-select strap (see README.md → Hardware)

**Pin note:** `PB13` is not the I2S2 CK pin on the F411 — the alternate-function 5 mapping is **PB10=CK, PB12=WS, PB15=SD**. Earlier plan had `SCK→PB13`; that is wrong for I2S2 and was corrected.

**Tasks:**
- [x] Configure I2S2 + DMA1 in CubeMX
- [x] Verify pin map (PB10/PB12/PB15, not PB13)
- [x] PLLI2S is configured in `HAL_I2S_MspInit` (PLLI2SN=192, PLLI2SM=25, PLLI2SR=2 → 96 MHz I2SCLK)
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
- PLLI2S is configured in `HAL_I2S_MspInit`, not in `SystemClock_Config` — the main PLL and PLLI2S are independent

**Deliverable:** 1 kHz sine tone audible on speaker, verified with online frequency meter. Phase 2 complete.

---

## Phase 3 — USB Audio Class 1.0 Device ✅

| Status | Task |
|--------|------|
| ✅ | **Complete — PC plays through MAX98357A; verified with `speaker-test -f 1000/2000/3000/4000`** |

**Goal:** PC sees the Black Pill as a USB speaker. Audio from PC plays through the board.

**Final working configuration:**
- Audio format advertised to PC: **44.1 kHz, 16-bit, mono**, implicit feedback (PC clock master)
- Isochronous OUT endpoint 0x01, **88-byte packets every 1 ms** (44 mono int16 samples)
- I2S2 audio frequency: `I2S_AUDIOFREQ_44K` (real ≈ 44.117 kHz, matching the USB clock)
- Pipeline: PC → USB OTG FS → `USB_DEVICE/App/usbd_audio_if.c` → `Core/Src/ring_buffer.c` (SPSC, 1024 int16 = 23 ms) → `Core/Src/audio_i2s.c` → DMA1 Stream 4 → I2S2 → MAX98357A → speaker
- I2S2 DMA half/cplt callbacks refill the just-played half from the ring; on underrun, the half is memset to silence so a brief stall pops cleanly
- TUTOR mode was active throughout: tutor wrote descriptors + ST-library glue + ring-buffer/I2S consumer skeleton; student wrote the ring-buffer functions and the I2S half-cplt/cplt refill callbacks. **The collaboration is recorded in the commit history, not in code comments.**

**Milestones — all met:**
- ✅ **3a — Enumerate:** `lsusb -v` shows `bInterfaceClass=1 Audio`, `ID 0483:5740 STM32 Audio Class`, `snd-usb-audio` registers on the host
- ✅ **3b — Capture:** USB isochronous OUT packets land in `audio_ring` (visible via `RingBuffer_Available()` from a debugger or printf)
- ✅ **3c — Pipe:** `speaker-test -D plughw:2,0 -c 1 -r 44100 -t sine -f 1000` plays a 1 kHz tone through the speaker. `aplay -D plughw:2,0 any.wav` plays music.

### The three-bug story — every one was a silent failure

All three bugs caused the device to **enumerate correctly but play no audio**. `lsusb` looked fine, `dmesg` looked fine, but the speaker was silent. Each took an evening to track down because none of them produced a hard error.

**Bug 1 — VBUS sensing blocked the D+ pull-up (no enumeration at all).**
- Symptom: `lsusb` showed nothing; `dmesg` showed the ST-Link V2 USB but not the Black Pill
- Root cause: `usbd_conf.c:342` had `hpcd_USB_OTG_FS.Init.vbus_sensing_enable = ENABLE;` (the CubeMX default). The OTG FS peripheral waited for VBUS B-session detection on PA9 before asserting the D+ pull-up. On the WeAct Black Pill, the PA9/VBUS line is wired to VBUS, but the OTG_FS VBUS sensing comparator isn't reliable enough on this board — it never sees the session, so D+ stays high-impedance and the host never sees the device.
- Fix: `vbus_sensing_enable = DISABLE` in `usbd_conf.c:342`. The D+ pull-up now asserts on `MX_USB_DEVICE_Init()` regardless of VBUS.
- Lesson: a board that *should* support VBUS sensing doesn't always do so reliably. If your F411 OTG_FS device doesn't enumerate and you have PA9 wired to VBUS, the first thing to try is `vbus_sensing_enable = DISABLE`.

**Bug 2 — Sample-rate mismatch: descriptor said 48 kHz, I2S ran at 44.1 kHz.**
- Symptom: device enumerated, `snd-usb-audio` registered, but `speaker-test` and `aplay` produced silence
- Root cause: `usbd_conf.h:78` had a hardcoded `#define USBD_AUDIO_FREQ 48000U`. The CubeMX `.ioc` set `USBD_AUDIO_FREQ=44100`, but the `#define` in `usbd_conf.h` was overriding the .ioc. So the descriptor advertised 48 kHz, `AUDIO_OUT_PACKET` was computed as 96 bytes (48 samples × 2 bytes), and the host sent 96-byte packets. But I2S2 was actually clocked at 44.1 kHz (PLLI2S at 96 MHz / 32-bit frame / 1.412 MHz BCLK → 44.117 kHz LRCLK). The ring was being filled with 48-sample chunks at a 48 kHz cadence, but the consumer pulled 44-sample chunks at a 44.1 kHz cadence → ring overran and underran in a way that read junk → silence.
- Fix: change `usbd_conf.h:78` to `#define USBD_AUDIO_FREQ 44100U`. The .ioc and the #define now agree; both the descriptor and the I2S clock are at 44.1 kHz; `AUDIO_OUT_PACKET` is 88 bytes (44 samples × 2); the host sends 88-byte packets; everything lines up.
- Lesson: ST's USB Audio template hardcodes the sample rate in `usbd_conf.h`. If you change the .ioc's USBD_AUDIO_FREQ, you must also change the `#define` — or better, delete the `#define` and read from the .ioc's setting.

**Bug 3 — `USBD_AUDIO_Sync` is exported by the ST library but never called from any internal code path.**
- Symptom: device enumerated, ALSA opened the device, but no audio came out and the ring was always empty
- Root cause: ST's `usbd_audio.c` exports `USBD_AUDIO_Sync` (the function that pushes the most recent USB packet into the user-side via `AUDIO_CMD_PLAY` → `AUDIO_AudioCmd_FS` → `RingBuffer_Write`). But the library has no internal call site for it — it's a "you must call this from your I2S callback" hook. Our `stm32f4xx_it.c` had the HAL I2S callbacks going straight to `AudioI2S_RefillHalfA/B` without first calling `HalfTransfer_CallBack_FS` / `TransferComplete_CallBack_FS` (the user-side wrappers that route to `USBD_AUDIO_Sync`). So `AUDIO_CMD_PLAY` was never issued, the ring never filled, and the consumer always read empty.
- Fix: in `stm32f4xx_it.c`, the HAL callbacks now do, in order: (1) `HalfTransfer_CallBack_FS()` / `TransferComplete_CallBack_FS()` to push the just-arrived USB packet into the ring, then (2) `AudioI2S_RefillHalfA/B()` to pull the next half from the ring into the DMA buffer. Order matters: refill from a populated ring, not an empty one.
- Lesson: any time you adopt a vendor library that exports a "you call this" symbol, grep the library for that symbol to see if the library itself calls it. If not, *you* are responsible for invoking it from the right ISR/callback. The ST USB Audio class driver assumes a SPSC model where the device-side ISR is the producer and the user-side ISR is the consumer; the library just plumbs the producer but expects you to plumb the consumer.

**What you'll have learned:**
- USB descriptors (bInterfaceClass, bNrChannels, bInterval, tSamFreq) and how they map to a real audio stream
- USB isochronous OUT endpoints and implicit-feedback clocking
- VBUS sensing on OTG FS — and when to disable it
- The ST USB Audio library's split-API model (class driver calls the user via `AUDIO_*_FS` callbacks; user calls the class driver via `USBD_AUDIO_Sync`)
- Lock-free SPSC ring buffers (used in every audio device ever)
- The I2S half/complete callback pattern (used in every DMA audio pipeline)
- How PC and embedded negotiate audio format, and what happens when they disagree

**Deliverable:** PC plays music through the Black Pill → speaker. Verified with `speaker-test` (1 kHz, 2 kHz, 3 kHz, 4 kHz tones) and `aplay` (WAV file). Phase 3 complete.

---

## Phase 3.5 — Reliability & Test Infrastructure ✅ (2026-09-17)

| Status | Task |
|--------|------|
| ✅ | Linker: CCMRAM region/section + stack bump |
| ✅ | Host-simulated ring-buffer unit tests (ASan-clean) |
| ✅ | Independent watchdog (IWDG) supervision |
| ✅ | Makefile convenience targets: `make flash / test / size` |

**Why:** before touching the Phase 4-6 feature stack (encoder, TFT, FreeRTOS),
the firmware needs (a) RAM headroom, (b) a regression net around the only
lock-free data structure in the build, and (c) a dead-device watchdog.

**Changes:**
- `STM32F411xx_FLASH.ld` — declared the 64 KB CCMRAM region (0x10000000), added
  a `.ccmram` (NOLOAD) output section for CPU-only data, bumped `_Min_Stack_Size`
  0x800 → 0x1000. Audio/DMA buffers must stay in main RAM (CCMRAM is CPU-only).
- `tests/` — new host-simulated suite for the USB→I2S SPSC ring buffer
  (8 tests, 5227 assertions): empty/full invariants, power-of-2 wraparound,
  partial ops, reset, 5000-iteration producer/consumer churn, and a
  global-stream integrity check across 200 full-ring wraps. `ring_buffer.c`
  compiles unmodified on the host. ASan/UBSan-clean. `make -C tests run`.
- `main.c` (all inside USER CODE) + `stm32f4xx_hal_conf.h` + Makefile — enabled
  `HAL_IWDG_MODULE_ENABLED`, linked `stm32f4xx_hal_iwdg.c`, started the
  windowless IWDG (~1 s, LSI/64, reload 500), refreshed every main-loop pass. A
  stuck main loop now ends in a reset, not a dead device.
- `Makefile` — `make flash` (st-flash), `make test`, `make size`.

**Debugging note:** the first three test runs "failed" — every time it was the
*test oracle*, not the ring: (1) a full-ring write sourced from a 256-byte array
(OOB read), (2) a read request sized above the destination buffer (OOB write),
(3) a stream oracle that restarted its pattern per write instead of treating the
samples as one contiguous stream. Fixed the oracles; the ring implementation was
never implicated. This is why host tests are worth having — they catch caller
contract violations cheaply, long before the ISA side would.

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
| ✅ | ST7735S driver over SPI DMA / GPIO |
| ✅ | CMSIS-DSP RFFT 256 + Hann window + 16 log-spaced frequency bands |
| ✅ | Display rendering (framebuffer bar chart + peak hold) |
| ✅ | USB enumeration timing fix (init visualizer after USB connect) |

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

## Deferred Items

| Status | Item | Notes |
|--------|------|-------|
| ⚠️ Corrected 2026-09-23 | `.ccmram` in linker script | Corrected — STM32F411 has no CCMRAM (0x10000000 causes BusFault). All data safely allocated in standard 128 KB SRAM (0x20000000). |
| ✅ 2026-09-17 | Bump stack 0x800 → 0x1000 | Done — makes room for `printf` (Phase 4 debug logs) and FreeRTOS (Phase 6). |
| ⏳ | Wire `AUDIO_VolumeCtl_FS` | Currently a no-op. Phase 4 connects the rotary encoder to this hook. |
| ⏳ | Sample rate drift compensation | I2S2 clock is ~44.1176 kHz vs USB 44.1000 kHz (-17.6 samples/s drift). Eventual ring underrun after ~2-5 min continuous stream without sample duplication/feedback endpoint. |
| ⏳ | STM32H743VIT6 Hardware Migration (Optional) | If dual clock domains / fractional PLL / SAI or internal DAC are desired, migrate to STM32H743VIT6 board. |
| ⏳ | 10-min playback stress test | Phase 3 acceptance test deferred. |

---

## Legend
- ⏳ Pending
- 🔄 In progress
- ✅ Complete
- ❌ Blocked
