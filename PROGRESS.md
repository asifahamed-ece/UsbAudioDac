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

## Phase 1 — Clock Tree (HSE → PLL → 48 MHz SYSCLK, PLLQ → 48 MHz USB, PLLI2S → 48 MHz I2S)

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
- **I2S audio:** PLLI2S at 48 MHz drives I2S2's internal prescaler; together with a Philips-standard 16-bit frame the HAL divider yields the requested 44.1 kHz LRCLK (real ≈ 44.117 kHz, +0.04% error)
- **System processing:** 48 MHz CPU is enough headroom for USB packets, audio buffering, and later RTOS tasks. The 60 MHz / 100 MHz designs considered earlier would work too, but 48 MHz keeps the bus dividers simple and the whole system at one frequency
- **No external I2S_CKIN:** early plan assumed an external 12.288 MHz crystal on PI0. Dropped because (a) PI0 is not brought out on the Black Pill header, and (b) PLLI2S at 48 MHz gives equivalent audio quality for our use case
- **Clock separation:** SYSCLK and I2SCLK are derived from independent PLLs, eliminating beat frequencies

**Clock tree math:**
- Main PLL: `VCO = (HSE/PLLM) × PLLN = (25/25) × 384 = 384 MHz`
- `SYSCLK = VCO / PLLP = 384/8 = 48 MHz`
- `USBCLK = VCO / PLLQ = 384/8 = 48 MHz`
- PLLI2S: `VCO_I2S = (HSE/PLLI2SM) × PLLI2SN = (25/25) × 192 = 192 MHz`
- `I2SCLK = VCO_I2S / PLLI2SR = 192/2 = 96 MHz`

**Note on earlier external-MCLK plan:** the F411 CEU6 package (UFQFPN48) does not expose PI0/I2S_CKIN, so the external 12.288 MHz crystal approach was abandoned. PLLI2S at 48 MHz gives the audio quality we need without the extra hardware.

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
- ✅ I2S2 Audio Frequency: I2S_AUDIOFREQ_44K (real ≈ 44.117 kHz from PLLI2S=48 MHz)
- ✅ I2S2 Clock Source: I2S_CLOCK_PLL (PLLI2S at 48 MHz)
- ✅ DMA1 Stream 4 for I2S2_TX (DMA channel 0), Circular mode, FIFO enabled, HALFWORD both sides
- ✅ DMA1_Stream4 IRQ priority 0,0 (NVIC enabled)
- ✅ HAL_I2S_MspInit is the sole PLLI2S config site (PLLI2SN=192, PLLI2SM=25, PLLI2SR=2) — SystemClock_Config does NOT touch PLLI2S
- ✅ MAX98357A wired: BCLK←PB10, LRCK/WS←PB12, SD←PB15; SD pin has L/R channel-select strap (see BOM.md)

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
- Root cause: `usbd_conf.h:78` had a hardcoded `#define USBD_AUDIO_FREQ 48000U`. The CubeMX `.ioc` set `USBD_AUDIO_FREQ=44100`, but the `#define` in `usbd_conf.h` was overriding the .ioc. So the descriptor advertised 48 kHz, `AUDIO_OUT_PACKET` was computed as 96 bytes (48 samples × 2 bytes), and the host sent 96-byte packets. But I2S2 was actually clocked at 44.1 kHz (PLLI2S at 48 MHz / 32-bit frame / 1.412 MHz BCLK → 44.117 kHz LRCLK). The ring was being filled with 48-sample chunks at a 48 kHz cadence, but the consumer pulled 44-sample chunks at a 44.1 kHz cadence → ring overran and underran in a way that read junk → silence.
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

## Deferred Items

| Status | Item | Notes |
|--------|------|-------|
| ✅ 2026-09-17 | Add `.ccmram` section to `STM32F411xx_FLASH.ld` | Done — reviewer flagged that the 64 KB CCMRAM at 0x10000000 was undeclared. Now declared via `.ccmram` (NOLOAD). Still NOT to be used for audio/DMA buffers (CCMRAM is CPU-only). Ready for the Phase 5 FFT scratch. |
| ✅ 2026-09-17 | Bump stack 0x800 → 0x1000 | Done — makes room for `printf` (Phase 4 debug logs) and FreeRTOS (Phase 6). |
| ⏳ | Wire `AUDIO_VolumeCtl_FS` | Currently a no-op. Phase 4 connects the rotary encoder to this hook. |
| ⏳ | 10-min playback stress test | Phase 3 acceptance test deferred. `speaker-test` for a few seconds and a short `aplay` were verified; need a long-duration run (no dropouts, no ring over/underruns, USB stays enumerated) before declaring Phase 3 production-ready. |

---

## Legend
- ⏳ Pending
- 🔄 In progress
- ✅ Complete
- ❌ Blocked
