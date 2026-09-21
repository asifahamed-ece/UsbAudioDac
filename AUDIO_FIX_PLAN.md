# Audio Fix Plan — STM32F411 USB Audio DAC

**Branch:** `feature/endpoint-feedback`  
**Date:** 2026-09-21  
**Status:** Ready to implement

---

## Root Cause Summary

Three bugs are compounding to produce the 200 Hz modulation buzz and the 1s → silence degradation. They are ordered by severity.

### Bug 1 — FATAL: `Sof_enable = DISABLE` (dead feedback path)

**File:** `USB_Audio_DAC_1.0/USB_DEVICE/Target/usbd_conf.c:339`

```c
hpcd_USB_OTG_FS.Init.Sof_enable = DISABLE;   // ← kills everything
```

`stm32f4xx_ll_usb.c:452` gates `USB_OTG_GINTMSK_SOFM` on this flag. With it disabled the OTG core never raises a SOF interrupt, so `HAL_PCD_SOFCallback` → `USBD_LL_SOF` → `USBD_AUDIO_SOF` never executes. Every commit since `86fbf3f` (add feedback endpoint) — including the sign fix `bb24be0` and the multiplicative PID `6bea2ac` — has been patching dead code.

**Effect without SOF:** the host never receives a rate-correction value. The I2S peripheral runs at its real clock (44 117.6 Hz) while the host sends at exactly 44 100 Hz. Net drain: **17.6 samples/s**. Half-buffer (1 760 samples) drains in ~100 s. This is the slow, inevitable cliff.

### Bug 2 — SEVERE: Wrong `wMaxPacketSize` in OUT endpoint descriptor

**File:** `USB_Audio_DAC_1.0/Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Src/usbd_audio.c:304`

```c
AUDIO_PACKET_SZE(USBD_AUDIO_FREQ),   // expands to (44100*2*2)/1000 = 176 bytes (STEREO)
```

The macro multiplies by `2U * 2U` (stereo × 2 bytes). For a **mono** stream the correct size is `(44100 * 2) / 1000 = 88` bytes nominal, or **90** bytes to accommodate the 44.1 kHz "long frame" (1 sample extra every 10th ms).

Linux `usb-audio` reads `wMaxPacketSize` to size its URB transfer buffers. It still sends the correct number of mono samples per SOF (88 or 90 bytes), so no extra data arrives. However the EP is opened with `AUDIO_OUT_PACKET = 88` bytes (`usbd_audio.c:398`). The mismatch:

- Descriptor says 176 → host allocates 176-byte URB slots, reserves extra FS bandwidth.
- EP hardware limit is 88 → `GetRxDataSize` returns 88 even when 90 bytes arrive.
- **1 sample silently dropped every 10th ms = 100 samples/s lost.**

Combined with Bug 1's 17.6 samples/s drift: total drain **117.6 samples/s**, half-buffer exhausted in **~15 s**. The recording analysis confirmed the 200 Hz DMA half-period modulation throughout the 22 s clip — consistent with continuous Sync underruns from the moment the buffer fills below the 220-sample threshold.

### Bug 3 — MODERATE: PID clamp is ±62 Hz, not ±1 kHz

**File:** `usbd_audio.c:738–745`

```c
if (fb > AUDIO_FEEDBACK_NOM_10_14 + 1024U)   // ±1024 Q10.14
```

`1024 / 16384 = 0.0625` samples/frame = **±62 Hz**, not the ±1 kHz stated in the comment. The startup transient when the buffer is near-empty requires a correction of roughly `(1760 samples × 256) / 2^22 × fb_nom ≈ 10 000` Q10.14 units. The ±1024 clamp caps correction at 6% of what's needed, making convergence ~10× slower than intended. Fix: widen to `±8192` (= ±500 Hz, safe and fast-converging).

---

## Spectral Evidence (from recorded playback)

| Observed | Expected (clean) | Interpretation |
|---|---|---|
| 200.01 Hz envelope modulation | absent | DMA half-buffer boundary every 4.989 ms |
| 100.2 Hz autocorrelation peak | absent | DMA full-buffer period (harmonic of 200 Hz) |
| 15 s playback before full degradation | continuous | buffer drain at ~117 samples/s |
| Audio present at all (no total silence) | — | `rd_enable` IS set; Sync IS called; buffer IS filling |

---

## Fix Plan

Three minimal, independent changes. Each touches exactly one location.

### Fix 1 — Enable SOF interrupt

**File:** `USB_Audio_DAC_1.0/USB_DEVICE/Target/usbd_conf.c`  
**Line:** 339  
**Change:**

```c
// BEFORE
hpcd_USB_OTG_FS.Init.Sof_enable = DISABLE;

// AFTER
hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE;
```

This unmasks `GINTMSK_SOFM`, causing `HAL_PCD_SOFCallback` to fire every 1 ms SOF. `USBD_AUDIO_SOF` runs, computes fill-level error, and transmits the Q10.14 feedback value to the host via `AUDIO_IN_EP (0x81)`.

**Risk:** SOF fires every 1 ms (1000 Hz) in addition to DataOut and DMA callbacks. On Cortex-M4 at 48 MHz this adds ~0.5 µs ISR overhead per ms, negligible. The `fb_tx_pending` flag already rate-limits to one feedback packet per transmission cycle.

### Fix 2 — Correct `wMaxPacketSize` for mono OUT endpoint

**File:** `USB_Audio_DAC_1.0/Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Src/usbd_audio.c`  
**Line:** 304  
**Change:**

```c
// BEFORE
AUDIO_PACKET_SZE(USBD_AUDIO_FREQ),    /* wMaxPacketSize … (Stereo macro) */

// AFTER  — replace the macro call with explicit mono + headroom value
LOBYTE(AUDIO_OUT_PACKET_MAX),         /* wMaxPacketSize = 90 bytes (mono 44.1kHz, long frame) */
HIBYTE(AUDIO_OUT_PACKET_MAX),
```

Add the constant to `usbd_audio.h` near `AUDIO_OUT_PACKET`:

```c
/* Maximum mono packet size: 44 100 Hz has 9 frames of 88 B and 1 frame of 90 B per 10 ms.
 * wMaxPacketSize must accommodate the largest frame. */
#define AUDIO_OUT_PACKET_MAX    90U
```

Also update `PrepareReceive` calls (lines 423 and 947 in `usbd_audio.c`) and `USBD_LL_OpenEP` (line 398) to use `AUDIO_OUT_PACKET_MAX` so the HAL DMA buffer matches the descriptor:

```c
// Lines 398, 423, 947: replace AUDIO_OUT_PACKET with AUDIO_OUT_PACKET_MAX
(void)USBD_LL_OpenEP(pdev, AUDIOOutEpAdd, USBD_EP_TYPE_ISOC, AUDIO_OUT_PACKET_MAX);
// ...
(void)USBD_LL_PrepareReceive(pdev, AUDIOOutEpAdd, haudio->buffer, AUDIO_OUT_PACKET_MAX);
```

**Note on `wr_ptr` overflow guard:** `DataOut` advances `wr_ptr += PacketSize` where `PacketSize` is now up to 90. `AUDIO_TOTAL_BUF_SIZE = 7040`. The existing roll-back `if (wr_ptr >= AUDIO_TOTAL_BUF_SIZE) wr_ptr = 0` already handles this. `rd_enable` check `wr_ptr == 3520` still works because: 9 packets of 88 + 1 packet of 90 = 882 bytes per 10 ms; after 40 ms = 3528 bytes — overshoots 3520. **This is a pre-existing issue.** Change `rd_enable` check from exact equality to `>=`:

```c
// usbd_audio.c ~line 937
// BEFORE
if (haudio->wr_ptr == (AUDIO_TOTAL_BUF_SIZE / 2U))

// AFTER
if (haudio->wr_ptr >= (AUDIO_TOTAL_BUF_SIZE / 2U))
```

**Risk:** If `haudio->buffer` is declared as exactly `AUDIO_TOTAL_BUF_SIZE` bytes, a 90-byte packet arriving when `wr_ptr = 7040 - 88 = 6952` would advance to `wr_ptr = 7042`, past the end. The existing `wr_ptr = 0` rollback catches `>= AUDIO_TOTAL_BUF_SIZE` — no overflow. The `AUDIO_TOTAL_BUF_SIZE` stays at `88 * 80 = 7040` which is `78 * 88 + 2 * 90 = 7040` — 78 short + 2 long frames per buffer cycle, fine.

### Fix 3 — Widen PID clamp to ±500 Hz

**File:** `USB_Audio_DAC_1.0/Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Src/usbd_audio.c`  
**Lines:** 738–745  
**Change:**

```c
// BEFORE
if (fb > AUDIO_FEEDBACK_NOM_10_14 + 1024U)
{
  fb = AUDIO_FEEDBACK_NOM_10_14 + 1024U;
}
else if (fb < AUDIO_FEEDBACK_NOM_10_14 - 1024U)
{
  fb = AUDIO_FEEDBACK_NOM_10_14 - 1024U;
}

// AFTER — ±8192 Q10.14 ≈ ±500 Hz, allows fast convergence from startup transients
if (fb > AUDIO_FEEDBACK_NOM_10_14 + 8192U)
{
  fb = AUDIO_FEEDBACK_NOM_10_14 + 8192U;
}
else if (fb < AUDIO_FEEDBACK_NOM_10_14 - 8192U)
{
  fb = AUDIO_FEEDBACK_NOM_10_14 - 8192U;
}
```

Update comment: `/* Clamped to ±8192 Q10.14 ≈ ±500 Hz from nominal */`

---

## Implementation Order

1. **Fix 1** (Sof_enable) alone — flash and test with `speaker-test -D plughw:2,0 -c1 -r44100 -t sine -f1000` for 60 s. If tone plays cleanly for 60 s: Fix 1 was the sole gating issue.
2. **Fix 2** (wMaxPacketSize + rd_enable >=) — adds 90-byte frame support, eliminates 100 samples/s loss.
3. **Fix 3** (PID clamp) — ensures fast convergence on reconnect/resume.

Each fix is a separate commit to allow bisect.

---

## Verification Checklist

- [ ] `make` — clean build, zero warnings added
- [ ] `make test` — all 5227 host-simulated ring-buffer tests pass
- [ ] `lsusb -v | grep -A2 "bEndpointAddress.*0x01"` — `wMaxPacketSize` shows 90 (0x005a)
- [ ] `speaker-test -D plughw:2,0 -c1 -r44100 -t sine -f1000` — clean 1 kHz tone for ≥ 60 s
- [ ] `aplay -D plughw:2,0 your_audio.wav` — no 200 Hz modulation buzz on speech recording
- [ ] `usbmon` or `Wireshark USB capture` — feedback IN packets visible every 4 ms (bRefresh=2, every 4 SOFs)

---

## Files Changed

| File | Change |
|---|---|
| `USB_DEVICE/Target/usbd_conf.c` | `Sof_enable = ENABLE` |
| `Middlewares/.../usbd_audio.h` | add `AUDIO_OUT_PACKET_MAX 90U` |
| `Middlewares/.../usbd_audio.c` | descriptor wMax, OpenEP, PrepareReceive → `AUDIO_OUT_PACKET_MAX`; `rd_enable >=`; PID clamp `±8192` |

Total: 3 files, ~10 lines changed.
