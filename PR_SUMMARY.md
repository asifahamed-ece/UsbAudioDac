# Audio Fixes: SOF Interrupt + Priority Inversion Resolution

## Summary
This PR merges critical audio stability fixes from `fix/sof-enable` into `main`. The firmware now plays YouTube audio with significantly improved quality, though some buzzing/frequency aliasing artifacts remain for future investigation.

## Changes Merged

### 1. **SOF Interrupt Enabled (Bug #1)**
- **File:** `USB_DEVICE/Target/usbd_conf.c`
- **Change:** Set `hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE`
- **Impact:** Enables async feedback endpoint to transmit rate corrections to the host

### 2. **Interrupt Priority Inversion Fixed**
- **Files:** `Core/Src/main.c`, `USB_DEVICE/Target/usbd_conf.c`
- **Change:**
  - DMA I2S: Priority 5 → **Priority 0** (highest)
  - USB OTG: Priority 0 → **Priority 3** (lower)
- **Impact:** Audio DMA callbacks now complete uninterrupted, eliminating 400Hz/300Hz modulation artifacts caused by USB SOF interrupts preempting buffer refills

### 3. **Documentation Added**
- **File:** `AUDIO_FIX_PLAN.md`
- **Content:** Multi-step debugging plan and root cause analysis

## Previous Issues Resolved

| Issue | Status |
|-------|--------|
| 400Hz/300Hz modulation artifacts in recordings | ✅ Resolved |
| 14.8% amplitude modulation causing warble | ✅ Resolved |
| 1kHz tone locking to 100Hz (wrong branch) | ✅ Resolved |
| USB enumeration working | ✅ Working |
| YouTube audio playback | ✅ Functional |

## Known Remaining Issues

1. **Buzzing with frequency aliases** — likely hardware-related (power supply ripple, ground loop, or MAX98357A decoupling)
2. **Audio not "cleanest"** — may require:
   - PID feedback controller from `feature/endpoint-feedback` branch
   - Hardware debugging (oscilloscope, power rail measurements)
   - Additional filtering or clock tuning

## Testing Performed

- ✅ USB device enumeration verified
- ✅ 1kHz sine wave playback tested
- ✅ YouTube video audio playback tested
- ✅ Spectral analysis of recordings (FFT + amplitude modulation)

## Architecture Notes

This branch maintains the **ring buffer architecture** (1024 samples = 23ms safety margin) which provides robustness against USB/I2S rate mismatches. The `feature/endpoint-feedback` branch explores a single-buffer direct pipeline with PID feedback but is not yet stable.

## Next Steps (Future Work)

1. Hardware debugging with oscilloscope (power rails, I2S signals, ground noise)
2. Evaluate need for PID feedback controller
3. Investigate remaining buzzing artifacts
4. Consider power supply filtering improvements

---

**Merge Commit:** `283c841`  
**Date:** 2026-09-22  
**Tested By:** @asifahamed-ece
