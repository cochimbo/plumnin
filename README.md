<p align="center"><img src="src/frontend/assets/icon.png" alt="The plumnin plum" width="128"></p>

# plumnin

A **Game Boy**, **Game Boy Color**, **NES** and **Super Nintendo** emulator for Windows
and Linux with a **pseudo-3D** effect that pulls the console's layers apart and puts them
at different depths. It is the Nintendo sibling of
[plumsg](https://github.com/cochimbo/plumsg), the Mega Drive emulator, and inherits its
frontend, its 3D view, its library, netplay and the tools around them.

## Status

**The Game Boy and the Game Boy Color are emulated**; the NES and the Super Nintendo come
next. A game made for the Color runs on a Color, the rest on a Game Boy with its green
screen. What there is:

- **Game Boy and Game Boy Color** (`src/core/gb/`): the SM83 processor cycle by cycle, the
  timer, the picture processor with its background, window and sprites kept apart for the
  3D view, the Color's palettes, VRAM banks and DMA, double speed, the sound (two square
  waves, wave and noise), and the cartridges with ROM only, MBC1, MBC2, MBC3 (with its
  clock, saved the way VBA-M and BGB save it) and MBC5. Cheats in Game Genie and GameShark
  codes, and RetroAchievements. It passes blargg's `cpu_instrs`, `instr_timing` and
  `mem_timing`, `dmg-acid2`, `cgb-acid2` and most of mooneye's tests (see Tests).

- The frontend from plumsg: the menus, the game library (covers, details and media from
  ScreenScraper and libretro-database, console tabs for the four consoles), savestates
  with thumbnails, rewind, run-ahead, fast forward and slow motion, cheats (with
  RetroArch's lists from internet), each game's own options and controls, RetroAchievements,
  GIF and video recording, console mode for playing from the sofa (Steam Deck, ROG
  Ally...), and the interface in Spanish, English, French and Portuguese.
- The 2D and pseudo-3D views, with the xBR and Anime4K upscalers and the scanline and
  CRT filters.
- Netplay over UDP with rollback.
- `plumnin-headless`, the emulator without a window, for tests, CI and reproducing bugs.
- A common interface for the consoles (`src/core/console.h`): each console only has to
  run its frames, draw its layers into a `VideoFrame` and describe its state. Savestates,
  run-ahead, netplay and the achievements are built on it once for all of them.
- A **test pattern** (`src/core/test_console.h`) that behaves like a console (a sprite the
  pads move, two planes, a tone), so that everything above can be tried and tested
  without any commercial ROM: `plumnin --test-pattern [gb|gbc|nes|snes]`.

Opening a NES or Super Nintendo game says that its console is not emulated yet. The plan,
in order:

1. The base. (done)
2. Game Boy and Game Boy Color. (done)
3. NES.
4. Super Nintendo.

What plumsg had that only made sense on Sega's consoles (the Mega CD and its BIOS, the
32X, the light guns, the Roland synthesizer with its instrument editor and score, and the
library's 3D shelf with the consoles' models) is not here.

## Building

You need CMake 3.16+, a C++17 compiler and SDL2 (if it is not installed, CMake downloads
it). Dear ImGui and rcheevos are downloaded by themselves.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/plumnin --test-pattern
```

`-DPLUMNIN_BUILD_FRONTEND=OFF` builds only the core, `plumnin-headless` and the core tests,
without SDL2 or OpenGL.

On Windows, the `windows-installer.yml` workflow builds the installer with Inno Setup (see
[installer/README.md](installer/README.md)). The same workflow builds the Linux AppImage
with `tools/linux/make-appimage.sh <build dir> <version>`.

## Tests

- `core_tests`: the savestates, rewind, run-ahead, netplay (two sessions over a simulated
  network that loses and reorders packets), RetroAchievements against a made-up server,
  the 3D planes, cheats, the library's names, ScreenScraper's and libretro-database's
  answers, on the test pattern; and the Game Boy on a small program the test writes itself
  (its picture and layers, the pad, the sound, savestates, cheats and the MBC3's save).
- `frontend_tests`: the settings files, each game's own options and the crash reports.
- `headless`: `plumnin-headless` end to end: hashes, savestates, input scripts, recordings
  and netplay between two processes.
- `i18n`: every text in the interface has its translation (`tools/i18n.py`).
- `gb_test_roms`: the Game Boy's test ROMs (blargg, mooneye, dmg-acid2, cgb-acid2), which
  are not part of plumnin. Unpack a release of
  [game-boy-test-roms](https://github.com/c-sp/game-boy-test-roms/releases) and configure
  with `-DGB_TEST_ROMS=path/to/it`; `gb-test-roms path/to/it --all` also runs the ones that
  do not pass yet, and `gb-test-roms --run ROM --png out.png` runs one to look at it.

```sh
./build/plumnin-headless --test-rom --system gb --frames 300 --do "60-120 right a" --png out.png
./build/plumnin-headless --help
```

## License

MIT (see [LICENSE](LICENSE)). plumnin does not include any game or BIOS. See
[PRIVACY.md](PRIVACY.md) for what it connects to.
