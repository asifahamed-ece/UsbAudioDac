# Debugging Story — how this firmware got to its current state

Every entry below is a bug that actually happened on this board, in roughly the
order it was found. Each one is written as **symptom → why it happened → fix**,
because the reasoning is the reusable part; the specific numbers are already
single-sourced in the code comments they refer to.

Current architecture and pin map: [README.md](README.md) and [WIRING.md](WIRING.md).
Active gotchas to read *before* touching this code: [AGENTS.md](AGENTS.md).

The recurring theme: **almost every bug here presented as "the data path is
broken" when the data path was fine and something upstream or downstream of it
was lying.**

---

## Part 1 — "It enumerates but there's no audio"

The USB device enumerated perfectly, `lsusb` showed the Audio class, and the
speaker stayed silent. Three independent causes, each hiding behind the others.

### 1.1 VBUS sensing on the Black Pill

**Symptom:** device never enumerated at all.

**Cause:** the F411's `USB_OTG_FS` peripheral can sense VBUS through the PA9
line, but on the Black Pill that pin is not reliably connected. The internal
comparator sat low, so the peripheral believed no cable was attached.

**Fix:** `hpcd_USB_OTG_FS.Init.vbus_sensing_enable = DISABLE` in
`USB_DEVICE/Target/usbd_conf.c` and let VBUS be ignored — the board is
permanently bus-powered anyway.

### 1.2 The sample rate mismatch that wasn't (at first)

**Symptom:** total silence.

**Cause:** three independent places declare the sample rate, and they disagreed.
`usbd_conf.h` had a hardcoded `#define USBD_AUDIO_FREQ 48000U`; the CubeMX
`.ioc` said 44100; and the I2S2 hardware was actually clocked at 44.1 kHz. The
descriptor advertised 48 kHz, so the host sent 48-sample packets, while the
consumer drained 44-sample chunks at a 44.1 kHz cadence. The ring overran and
underran in a way that read as junk.

**Fix (first pass):** make all three agree. At this point the device ran
"44.1 kHz" — and it produced sound.

### 1.3 …and then the rate that was *almost* right

This is the one that cost the most time, because the audio was *mostly* fine.

**Symptom:** periodic soft thuds / clicks, roughly every second or two, on an
otherwise clean sustained tone. Easy to mistake for a bad USB cable or a loose
speaker connector. A 10-second listen did not show it.

**Cause:** the device claimed 44 100 Hz, but the real I2S2 rate was
**44 117.647 Hz** — the `PLLI2S` divisors available from a 25 MHz HSE cannot hit
44 100 exactly, and the nearest was +400 ppm off. Being *fast* matters: the I2S
consumed ~17.6 samples more per second than USB delivered, so the ring drained
gradually and every DMA refill eventually came up short. The deficit surfaced as
a zero-gap, i.e. a thud.

**Why it hid:** the error is tiny per second. It only becomes audible once the
ring has drained far enough to miss, which took minutes of continuous playback.
It was finally caught by recording a long stretch (61.9 s) with a phone mic and
listening back.

**Fix:** stop approximating. An exhaustive search over every legal
`PLLI2S_M/N/R` confirmed that **44 100 Hz is mathematically unreachable** from a
25 MHz HSE — it would require `I2SCLK` to be an exact multiple of 1 411 200 Hz,
which no small-integer ratio of 25 MHz can produce. So the target moved to a rate
that *is* reachable:

| | Discarded | Adopted |
|---|---|---|
| PLLI2S | 96 MHz (`M=25, N=192, R=2`) | **192 MHz** (`M=25, N=384, R=2`) |
| HAL config | `I2S_AUDIOFREQ_44K` (`I2SDIV=34, ODD=0`) | **`I2S_AUDIOFREQ_48K`** (`I2SDIV=62, ODD=1`) |
| Real rate | 44 117.647 Hz (+400 ppm) | **48 000.000 Hz exact** |
| USB packet | 88 bytes, 90 every 10th frame | **96 bytes, every frame, no alternation** |
| Drain rate | 17.6 samples/s | **0.000 samples/s** |

`96 MHz → 192 MHz` was also within spec (`fPLLI2S_OUT` max 216 MHz, VCO 384 MHz
inside 100–432 MHz). Two other exact-48 kHz solutions (`N=384,R=5` → 76.8 MHz and
`N=192,R=5` → 38.4 MHz) were rejected as low-divider / non-canonical.

**Approach considered and deliberately dropped:** a USB **synchronous-feedback
endpoint** (10.14 format) to measure the host's real clock and correct for the
error, keeping 44.1 kHz. That was prototyped in full — endpoint sizing, the
`Q10.14` clamp, `Sof_enable`, the `wr_ptr` overflow analysis — and then abandoned.
It adds a whole subsystem (endpoint, control requests, SOF plumbing) purely to
correct an error that doesn't exist once the rate is exact. Once
`USBD_AUDIO_FREQ`, the `.ioc`, the I2S divisor and the descriptor all agreed on
48 000, the feedback path became dead weight. Two of its ideas *did* survive and
are still load-bearing: `Sof_enable = ENABLE`, and the `AUDIO_OUT_PACKET_MAX`
spill pad described in §2.1.

### 1.4 `USBD_AUDIO_Sync` exists but is never called

**Symptom:** silence again, after the rate was correct.

**Cause:** ST's audio class *exports* `USBD_AUDIO_Sync` and documents it as the
thing that hands buffered USB data to the application — but the library itself
never calls it. The data sat in `haudio->buffer` and was never moved to the ring.

**Fix:** call it from the I2S DMA callbacks in `Core/Src/stm32f4xx_it.c`. The
*order* is load-bearing:

```
DMA1_Stream4_IRQHandler
  1. HalfTransfer_CallBack_FS()  -> USBD_AUDIO_Sync() -> RingBuffer_Write()   [producer]
  2. AudioI2S_RefillHalfA()      -> RingBuffer_Read()                         [consumer]
```

Sync first, or the consumer drains a ring the producer hasn't refilled yet.

---

## Part 2 — Crashes, screeching and modulation

### 2.1 The buffer overrun that ate its own struct

**Symptom:** loud screeching, then a hard fault; the device dropped off USB.

**Cause:** the USB audio transfer buffer is a *circular* buffer, but the HAL
writes each received packet **linearly** to `&buffer[wr_ptr]`. At 44.1 kHz the
packets were 88 bytes normally and 90 on every 10th frame ("long frame"), so
`wr_ptr` was not always packet-aligned. A packet armed near the logical end
(7 040 bytes) ran past the end of the array and wrote directly over the
`rd_ptr` / `wr_ptr` / `control` fields *immediately following it in the struct*.

**Fix, two parts:**
- Give `haudio->buffer[]` a **spill pad** of `+ AUDIO_OUT_PACKET_MAX` bytes past
  its logical end, so an overshoot lands in padding instead of in the struct.
- Wrap with `wr_ptr -= AUDIO_TOTAL_BUF_SIZE` rather than snapping to `0`, so the
  spare capacity is actually used.

**Still true at 48 kHz, for a different reason:** with 96-byte packets and
`7680 % 96 == 0`, `wr_ptr` stays aligned and a *full* packet can no longer
overshoot. But a **short or zero-length packet** (a poll carrying fewer fresh
bytes than a full frame) can still leave `wr_ptr` off-grid — worst case
`wr_ptr = 7679`, copy ending at 7775, which is exactly the declared array size.
The pad is not optional; only the *reason* for it changed.

### 2.2 Priority inversion: the USB SOF was outranking the audio refill

**Symptom:** ~400/300 Hz amplitude modulation riding on the music.

**Cause:** the I2S refill runs in `DMA1_Stream4_IRQHandler`, and the USB SOF
interrupt also fires ~1000×/s. The SOF was at a *higher* priority than the DMA
refill, so the refill got preempted mid-half, and the DMA replayed a
partially-refilled buffer.

**Fix:** `DMA1_Stream4_IRQn` → priority 0, `OTG_FS_IRQn` → priority 3. The
refill now completes uninterrupted. `Sof_enable = ENABLE` was set at the same
time.

---

## Part 3 — The residual distortion that wasn't firmware at all

**Symptom:** with everything above fixed, playback still had a persistent
low-level ~200/300 Hz buzzy character. No amount of ring-buffer, DMA or
clock work touched it.

**How it was actually isolated:** the `dbg_bypass_usb` diagnostic in
`audio_i2s.c` can replace USB audio with a generated 1 kHz tone while still
driving the *identical* `DMA1_Stream4 → I2S2 → MAX98357A → speaker` path. With
the generated tone the buzzy character **was still there** — which exonerated the
entire USB/ring/software chain in one test and pointed at the amplifier.

**Cause:** the MAX98357A's `GAIN` pin was left floating. The datasheet calls
floating the 9 dB "factory default", but the spec is not a measurement, and on
this module the floating pin was evidently picking up noise.

**Fix:** tie **GAIN to GND** (12 dB). The distortion disappeared completely.

**Lesson worth keeping:** when a sound survives a change that replaces the
entire digital signal path, the remaining suspect is the analogue path. The
generated-tone test existed precisely to make that one-step check.

---

## Part 4 — The display: eight bugs, all of them invisible in code review

The TFT work produced by far the highest bug-per-line ratio in the project. Every
one of these rendered fine logically and wrong on the glass.

### 4.1 A second font, drawn transposed

Tried adding a 5x7 font (renderer + unit test) alongside the existing 8x8 one, to
match some layout maths. It rendered as scrambled glyphs on the panel. Rather
than debug a second renderer, the 5x7 font, its renderer and its test were
**deleted**. There is now exactly one font, `Core/Src/font8x8.h`.

### 4.2 The rate label that couldn't be built from a macro

Tried to derive the header's rate label from `USBD_AUDIO_FREQ` with the
preprocessor. It printed the literal text `48000U / 1000`, because `#` does not
macro-expand its argument. Before that attempt, the label had been a hardcoded
`"44.1k"` that silently went stale when the device moved to 48 kHz.

**Fix:** build the digits at runtime into a small buffer
(`build_rate_stamp()`). It cannot go stale, and it cannot print an expression.

### 4.3 Labels clipped by rows the panel never shows

The bottom **3 logical rows (125–127) are physically invisible**: the driver
offsets rows by `ST7735_ROWSTART = 3` and `MADCTL` applies no rotation
compensation, so those rows land outside the panel's active window and are
discarded. The region labels sat at `y=120`, so their last 3 rows vanished and
the strip rendered half-cut — with **no error anywhere**, because 127 is a
perfectly legal value for `ST7735_HEIGHT`.

**Fix:** drop the bar cap from 90 px to 84 px (exactly 12 LED blocks of 7, which
also removed a 6 px dead row above a full column) and move the labels to `y=112`.
Two compile-time guards (`label_strip_must_fit`, `header_texts_must_not_collide`)
now fail the *build* rather than the display.

### 4.4 `FillRect` doesn't clip, and that asymmetry bit us

`ST7735_DrawPixel` bounds-checks each pixel. `ST7735_FillRect` — and the
`DrawHLine`/`DrawVLine` wrappers over it — do **not**: they only reject
`w <= 0` / `h <= 0`, then program a GRAM window verbatim. The caller owns the
bounds. This is now documented in `st7735.h`.

### 4.5 The status line that garbled its own words

Rendered as `CIDNAЯS` and `UITT TЯS`. Cause: consecutive status words have
different lengths, and centring a short word over a longer one leaves the longer
word's end pixels behind. Measured at up to **30 stray pixels per transition**.

**Fix:** erase the whole 8-row band before every redraw, and only redraw when the
word actually changes. Each word is held 5 ticks (~275 ms) so it can be read.
Verified 0 stray pixels across all 11 transitions.

### 4.6 A cone instead of a sun

The sun disc's per-row half-width is the widest `hw` with `hw² + dy² ≤ r²`.
Implemented first with an integer Newton square root
(`sqrt(x) ≈ (b + x/b)/2`). At the disc's poles `x` is 0, so `x/b` is 0, the
iteration collapses toward 0 instead of settling, and the top of the disc was
eaten away — a cone.

**Fix:** walk `hw` outward with an exact integer test (≤ 14 iterations per row,
no division at all). The code carries an explicit "do NOT replace this with
Newton sqrt" warning so the next person doesn't rediscover the cone.

### 4.7 Scanlines that tinted the black sky navy

CRT scanlines were painted across the full screen width for the retro look. On a
near-black sky they didn't read as scanlines — they tinted the whole upper half
navy and turned it into a striped rectangle. **Removed.** The banding is now
confined to the sun disc, where it's a deliberate sunset effect. A glow row just
above the horizon was dropped for the same reason (it striped the disc's lower
bands).

### 4.8 The sun's gold band, rendered underwater

The sun colour ramp was computed across the disc's *full* height. But with the
sun setting, 11 of its 28 rows sit below the horizon and are painted over by the
ground cover — so the warm gold band landed at depths 26–27, entirely hidden, and
the sun came out with only two colours.

**Fix:** split the *visible* height into thirds, putting the gold right at the
horizon where a sunset is actually brightest.

### 4.9 The grid that emptied itself halfway through

The perspective grid advanced one row per animation step and erased the row
behind it. On the outer rows that ran off the bottom of the band and started
deleting the rows still to come — the grid visibly emptied out mid-splash.

**Fix:** repaint all `GRID_ROWS` lines every tick (~1.5 KB, ~1 ms at 12 MHz) with
a travelling highlight, so each frame is a complete, self-consistent picture. The
correct version also turned out to be the cheap one.

### 4.10 Boot time vs. USB enumeration

`Visualizer_Init()` runs *after* `MX_USB_DEVICE_Init()`, so a slow boot eats into
the host's enumeration window. There's a reverted commit in the history —
"move Visualizer_Init after USB enumeration to avoid host timeout" — which is
exactly that failure. The splash is paced at 54 ticks × 55 ms + a 500 ms hold
(≈ 3.5 s) against a 5 s budget, with `BOOT_STEP_DELAY_MS` as the single dial if
enumeration ever gets flaky.

---

## Part 5 — A linker script that was a booby trap

`STM32F411xx_FLASH.ld` shipped with a `CCMRAM` region at `0x10000000` and an empty
`.ccmram` section, inherited from an F4x7-family template, with a comment
inviting `__attribute__((section(".ccmram")))`.

**The problem:** the STM32F411 has **no** CCM data SRAM. That block exists on the
F405/407/415/417/427/429/437/439/469/479 family but not on the
F401/410/411/412/413. ST's own linker templates confirm it — they define a
`CCMRAM` region for the former and not the latter. On this part `0x10000000` is
unmapped, so one tagged variable would HardFault on first touch, **and the link
would still succeed**, so nothing would warn you.

**Fix:** both the region and the section are removed, so a stray tag now fails
the link instead of building a trap. A comment records why, and points at the
ST template to copy from if this is ever ported to a part that does have CCM.

---

## Still open (deliberately)

| Item | Why it's still open |
|------|--------------------|
| `RingBuffer_Reset()` torn-state race | Reached on the control path at NVIC priority 3, while the priority-0 I2S DMA handler can preempt it between its two stores. Worst case is a click replaying stale audio at stream start — not corruption. Fixing it means masking interrupts around the stores, which is its own risk. Documented in `ring_buffer.c` and AGENTS.md. |
| `audio_fft.c` `band_start[]` Hz labels | The table holds *bin indices*, which the rate change left alone, so the bands now label ~8.8 % higher in Hz than the original design spec. Cosmetic; rescaling would move the bands and change which band a 1 kHz tone lands in (see the note in `tests/test_audio_fft.c`). |
| 10-minute playback stress test | Phase 3 acceptance, never run end-to-end. |
| `AUDIO_VolumeCtl_FS` | Still a no-op; Phase 4 (encoder volume) will wire it. |

---

## What actually generalises

1. **A device that enumerates proves almost nothing.** Three separate "no audio"
   bugs lived behind one symptom. Check the clock chain, then the data handoff,
   then the amplifier — in that order.
2. **Rate mismatches hide as periodicity, not as silence.** A tiny error
   (±400 ppm) is inaudible per second and manifests minutes later as a drain.
   Record long sessions; short tests are actively misleading here.
3. **When it can't be the software, prove it in one test.** The generated-tone
   bypass exonerated the entire digital chain in a single run and pointed at the
   amp.
4. **Prefer the exact solution over the correcting one.** The feedback endpoint
   was a legitimate design that became unnecessary once the rate was made exact.
5. **Turn invisible display constraints into build failures.** The 3 hidden rows
   and the header collision are now compile-time array-size assertions, not
   things you squint at on glass.
6. **When replacing a signal path is cheaper than debugging it, replace it.** The
   5x7 font was deleted rather than fixed.
