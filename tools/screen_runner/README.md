# screen_runner

Interactive desktop runner for SeedSigner C-module screens.

Status: **planned / in implementation**
Scope: interactive desktop harness for SeedSigner C-module screens using scenario definitions.

## Purpose

Provide a fast local loop for live screen interaction without ARMv6 emulation.

Primary goals:
- load and run existing C-module screens on desktop
- reuse `../scenarios.json`
- support keyboard navigation input testing
- allow quick switching between `screen / variation` scenarios

## Planned behavior

- Top runner chrome with a single scenario selector (`screen / variation`)
- Embedded SeedSigner screen viewport below the chrome
- Keyboard mapping:
  - arrows -> directional nav
  - Return/Enter -> select/press
  - `1`/`2`/`3` -> KEY1/KEY2/KEY3
- After scenario load, focus shifts into rendered screen automatically

---

## OS-specific setup

## macOS (active target)

### Dependencies

- Xcode Command Line Tools
- CMake
- SDL2
- libpng
- (optional) ImageMagick

### Install (Homebrew)

```bash
xcode-select --install
brew install cmake sdl2 libpng imagemagick
```

> `imagemagick` is optional for this runner itself, but useful if screenshot/gif flows are used from the same environment.

### Build steps

TBD (to be finalized with actual runner target and CMake options)

### Run steps

TBD (to be finalized with actual executable path/flags)

---

## Linux

### Dependencies

TBD

### Build steps

TBD

### Run steps

TBD

---

## Windows

### Dependencies

TBD

### Build steps

TBD

### Run steps

TBD

---

## Notes

- Scenario source of truth: `../scenarios.json`
- This doc will be updated as soon as the first macOS runner target lands.
