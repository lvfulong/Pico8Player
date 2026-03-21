# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Pico8Player ("Pico Pico") is a hardware/software PICO-8 game engine running on microcontrollers (ESP32, Raspberry Pi Pico) and desktop (SDL). It implements the PICO-8 API in C, runs games via an embedded z8lua VM (Lua 5.2 with PICO-8 dialect and fixed-point math), and supports 7 platform backends.

## Build Commands

### PC/SDL (primary development target)
```bash
# Prerequisites: cmake, g++, libsdl2-dev
git submodule update --init
mkdir pc_pico && cd pc_pico
cmake -DBACKEND=PC ..
make
```

### Windows
```bash
bash build_windows.sh
```

### Tests
```bash
mkdir build && cd build
cmake -DBACKEND=TEST ..
make
make test
# Regenerate golden files: OVERWRITE_TEST_BUF=1 make test
```

### Cartridge preprocessing (required before build)
```bash
# Converts .p8 cartridges → src/generated/static_game_data.h
# Compiles Lua to bytecode, extracts gfx/map/sfx sections
python3 scripts/to_c.py
```

### Static analysis
```bash
make cppcheck
```

## Architecture

**Execution flow:** `main.cpp` → init video/audio/engine → menu selection → `cartParser()` → `init_lua()` (load stdlib + bytecode) → game loop (`_update()` → `_draw()` → `flip()` + GC step)

**Key source files:**
- `src/main.cpp` — Entry point, game loop orchestration
- `src/pico8api.c` — PICO-8 drawing/input/math API implementation (~1150 LOC, the largest file)
- `src/engine.c` — Lua VM lifecycle, stdlib loading, frame timing, API function registration
- `src/backend.h` — Platform abstraction interface (each backend implements: init_video, init_audio, handle_input, flip, get_time)
- `src/data.h` — Core data structures: GameCart, DrawState, SFX, Channel, Spritesheet
- `src/synth.c` — Sound synthesis (adapted from zepto8)
- `stdlib/stdlib.lua` — PICO-8 standard library (all, add, del, count, etc.)

**Backend implementations:** `sdl_backend.c` (PC), `pico_backend.c` (RPi Pico), `esp/backend.c` (ESP32), `rawdraw_backend.c`, `test_backend.c`

**Build pipeline:** `.p8` cartridge → `scripts/to_c.py` parses sections (`__lua__`, `__gfx__`, `__map__`, `__sfx__`, etc.) → compiles Lua via `luac` → generates `src/generated/static_game_data.h` with bytecode + binary assets as C arrays.

**Memory layout:** Spritesheet 16KB + Fontsheet 16KB + Map 8KB + Front buffer 8KB + Back buffer 32KB + Lua heap ~2MB.

## CMake Backend Options

Set via `-DBACKEND=<TYPE>`: `PC`, `PICO`, `ESP32`, `RAWDRAW`, `ANDROID`, `THREEDS`, `TEST`

## Testing

Tests are in `tests/` — C++ files that render frames and compare against golden `.bin` buffers in `tests/data/`. Lua regression tests use cartridges in `tests/regression/`. Use `tests/buf_to_png.py` to visualize buffer outputs.

## API Implementation Status

Graphics mostly implemented. Notable gaps: `fillp()`, `tline()`, `cursor()` not implemented. `pal()` only supports draw palette. `music()` not implemented. `sfx()` partial. `cartdata` persistence not implemented. See README.md for the full API support matrix.
