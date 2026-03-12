# screen_runner

Interactive desktop runner for SeedSigner LVGL screens. Provides a fast local iteration loop without target hardware.

## Features

- Scenario sidebar — click any row to load it instantly
- Scenario variations listed under their parent screen name
- Hardware keyboard navigation
- Status bar shows button selection actions

## Controls

These apply on all platforms.

**Keyboard**
- Arrow keys, Enter — forwarded to LVGL hardware nav
- `1` / `2` / `3` — mapped to ENTER (KEY1/KEY2/KEY3 compatibility)
- `PageUp` / `[` / `,` — load previous scenario
- `PageDown` / `]` / `.` — load next scenario
- `T` — toggle input mode (Keyboard / Mouse)

**Mouse**
- Click any row in the sidebar to load that scenario
- Scroll wheel over the sidebar scrolls the scenario list

---

## macOS

### Dependencies

- Xcode Command Line Tools
- CMake
- SDL2
- SDL2_ttf
- ImageMagick (recommended — required for the title bar logo; build still succeeds without it but the logo will not appear)

### Install (Homebrew)

```bash
xcode-select --install
brew install cmake sdl2 sdl2_ttf imagemagick
```

### LVGL setup (fresh clone)

If `LVGL_ROOT` is not already available from your local build environment, clone LVGL locally and pin to the project target tag.

```bash
# from repo root
mkdir -p third_party
git clone https://github.com/lvgl/lvgl.git third_party/lvgl
cd third_party/lvgl
git checkout <LVGL_TARGET_TAG>
```

> Note: this step may be unnecessary if you already have a compatible LVGL tree and pass it via `-DLVGL_ROOT=...`.

### Build

```bash
cmake -S tools/screen_runner \
      -B tools/screen_runner/build \
      -DLVGL_ROOT="$PWD/third_party/lvgl" \
      -DCMAKE_PREFIX_PATH="$(brew --prefix sdl2):$(brew --prefix sdl2_ttf)"
cmake --build tools/screen_runner/build -j
```

### Run

```bash
tools/screen_runner/build/screen_runner [tools/scenarios.json]
```

---

## Linux

### Dependencies

- CMake ≥ 3.16
- GCC or Clang with C++17 support
- SDL2 dev package
- SDL2_ttf dev package
- ImageMagick (recommended — same caveat as macOS)

### Install (Debian / Ubuntu)

```bash
sudo apt update
sudo apt install cmake build-essential libsdl2-dev libsdl2-ttf-dev imagemagick
```

### Install (Fedora / RHEL)

```bash
sudo dnf install cmake gcc-c++ SDL2-devel SDL2_ttf-devel ImageMagick
```

### LVGL setup (fresh clone)

```bash
# from repo root
mkdir -p third_party
git clone https://github.com/lvgl/lvgl.git third_party/lvgl
cd third_party/lvgl
git checkout <LVGL_TARGET_TAG>
```

### Build

```bash
cmake -S tools/screen_runner \
      -B tools/screen_runner/build \
      -DLVGL_ROOT="$PWD/third_party/lvgl"
cmake --build tools/screen_runner/build -j
```

If CMake cannot auto-detect SDL2 or SDL2_ttf, add:

```bash
-DCMAKE_PREFIX_PATH="/usr"
```

### Run

```bash
tools/screen_runner/build/screen_runner [tools/scenarios.json]
```

---

## Windows

The recommended path on Windows is **MSYS2 with the MINGW64 environment**, which provides a Unix-like shell and the same SDL2 packages used on macOS/Linux.

### Install MSYS2

1. Download and install MSYS2 from [msys2.org](https://www.msys2.org).
2. Open the **MSYS2 MINGW64** shell (not UCRT64 or CLANG64).

### Install dependencies

```bash
pacman -Syu
pacman -S mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-SDL2 \
          mingw-w64-x86_64-SDL2_ttf \
          mingw-w64-x86_64-imagemagick
```

### LVGL setup (fresh clone)

```bash
# from repo root
mkdir -p third_party
git clone https://github.com/lvgl/lvgl.git third_party/lvgl
cd third_party/lvgl
git checkout <LVGL_TARGET_TAG>
```

### Build

Run from the repo root inside the MSYS2 MINGW64 shell:

```bash
cmake -S tools/screen_runner \
      -B tools/screen_runner/build \
      -G Ninja \
      -DLVGL_ROOT="$PWD/third_party/lvgl" \
      -DCMAKE_PREFIX_PATH="/mingw64"
cmake --build tools/screen_runner/build -j
```

### Run

```bash
tools/screen_runner/build/screen_runner.exe [tools/scenarios.json]
```

> The SDL2 DLLs (`SDL2.dll`, `SDL2_ttf.dll`) must be on `PATH` or copied next to the binary. MSYS2 installs them to `/mingw64/bin`; the simplest fix is to run from the MSYS2 MINGW64 shell, which already includes that directory in `PATH`.

---

## Notes

- Scenario source of truth: `tools/scenarios.json` (relative to repo root)
- OpenSans fonts and the SeedSigner logo are sourced from `components/seedsigner/assets/` and copied to the build directory at build time by CMake post-build steps.
