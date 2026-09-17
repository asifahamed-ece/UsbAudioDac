# Changelog

All notable changes to the STM32F411 USB Audio DAC firmware.

## [Unreleased]

### 2026-09-17 — Reliability & test infrastructure (Phase 3.5)

- **Linker** (`USB_Audio_DAC_1.0/STM32F411xx_FLASH.ld`): declared the 64 KB
  CCMRAM region (0x10000000) with a `.ccmram` (NOLOAD) section for CPU-only
  data; bumped `_Min_Stack_Size` from 0x800 to 0x1000.
- **Tests** (`USB_Audio_DAC_1.0/tests/`): new host-simulated unit suite for the
  SPSC ring buffer — 8 tests / 5227 assertions, clean under ASan/UBSan.
  Run with `make -C tests run`. No ARM toolchain needed.
- **Watchdog**: enabled `HAL_IWDG_MODULE_ENABLED`, linked the IWDG HAL driver,
  and started a windowless IWDG (~1 s) in `main.c`, refreshed each main-loop
  pass. A stuck main loop now ends in a reset instead of a dead device.
- **Build**: added `make flash`, `make test`, and `make size` convenience
  targets to the firmware Makefile.

### Deferred items resolved (2026-09-17)

- `.ccmram` linker section — done.
- Stack 0x800 → 0x1000 — done.

## [0.x] — Phases 0–3 (historical)

- Phase 0: toolchain, board bring-up, LED blink.
- Phase 1: clock tree (HSE 25 MHz → 48 MHz SYSCLK/USB, PLLI2S → 96 MHz I2S).
- Phase 2: I2S2 + DMA, 1 kHz sine verified on speaker.
- Phase 3: USB Audio Class 1.0 device — PC streams 44.1 kHz/16-bit audio through
  the board to the MAX98357A. Three silent-failure bugs chased down and
  documented in `PROGRESS.md`.