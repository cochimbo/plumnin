<p align="center"><img src="src/frontend/assets/icon.png" alt="The plumnin plum" width="128"></p>

# plumnin

A **Game Boy**, **Game Boy Color**, **NES** and **Super Nintendo** emulator for Windows
and Linux with a **pseudo-3D** effect that pulls the console's layers apart and puts them
at different depths. It is the Nintendo sibling of
[plumsg](https://github.com/cochimbo/plumsg), the Mega Drive emulator, and inherits its
frontend, its 3D view, its library, netplay and the tools around them.

## Status

**The Game Boy, the Game Boy Color, the NES and the Super Nintendo are emulated.** A game
made for the Color runs on a Color, the rest on a Game Boy with its green screen. What
there is:

- **Game Boy and Game Boy Color** (`src/core/gb/`): the SM83 processor cycle by cycle, the
  timer, the picture processor with its background, window and sprites kept apart for the
  3D view, the Color's palettes, VRAM banks and DMA, double speed, the sound (two square
  waves, wave and noise), and the cartridges with ROM only, MBC1, MBC2, MBC3 (with its
  clock, saved the way VBA-M and BGB save it) and MBC5. Cheats in Game Genie and GameShark
  codes, and RetroAchievements. It passes all of blargg's tests but `oam_bug` (`cpu_instrs`,
  `instr_timing`, `mem_timing`, `halt_bug`, `interrupt_time`, `dmg_sound`, `cgb_sound`),
  `dmg-acid2`, `cgb-acid2` and all of mooneye's acceptance and cartridge tests for the
  models it emulates (see Tests).
- **NES** (`src/core/nes/`): the 2A03's 6502 cycle by cycle (with every unofficial
  instruction, the dummy reads and writes, and the sprite and DMC DMAs that halt it), the
  PPU dot by dot with its background and sprites kept apart for the 3D view (and an option
  to lift the limit of 8 sprites a line), NTSC and PAL (from the header or the region
  chosen), the sound (two pulses, triangle, noise and DMC), and the mappers NROM, MMC1,
  UxROM, CNROM, MMC3 (MMC3A and MMC6 too), AxROM, MMC2, MMC4, Color Dreams, BNROM /
  NINA-001, GxROM, Camerica, UNROM 180, Sunsoft FME-7 (without its sound) and Namco 108,
  from iNES and NES 2.0 files. Battery saves, cheats in Game Genie and Pro Action Replay
  codes, and RetroAchievements. It passes the 140 test ROMs of the suite in Tests:
  blargg's processor, PPU, APU and DMA tests (also on PAL), the sprite 0 hit and overflow
  tests, `ppu_vbl_nmi` and the MMC3's.
- **Super Nintendo** (`src/core/snes/`): the 65C816 bus cycle by bus cycle (with the
  memory's 6, 8 and 12 clock speeds, FastROM and the WRAM refresh), the DMA and HDMA, the
  multiplier and divider, the H/V timers and the automatic pad read; the PPU a line at a
  time with its eight modes (mode 7 with EXTBG, offset per tile, the high resolution of
  modes 5 and 6 and pseudo high resolution), the sprites with their limits of 32 a line
  and 34 tiles (and the option to lift them), the windows, mosaic, colour math with the
  sub screen and the brightness, its backgrounds and sprites kept apart for the 3D view;
  the sound module with the SPC700, its timers and IPL, and the DSP (BRR, Gaussian
  interpolation, ADSR and GAIN, noise, pitch modulation and echo); LoROM, HiROM and
  ExHiROM cartridges (with or without a copier's header), NTSC and PAL. Battery saves,
  cheats in Game Genie and Pro Action Replay codes, and RetroAchievements. The processors
  pass all of SingleStepTests' tests (147 100 of the 65C816 and 256 000 of the SPC700,
  with every bus cycle), and krom's processor and bank test ROMs (see Tests).

  Of the chips in the cartridges, NEC's signal processors: the DSP-1 (Super Mario Kart,
  Pilotwings...), DSP-2, DSP-3 and DSP-4, and the ST010 and ST011, run their own
  program, instruction by instruction. That program is in the chip and does not come
  with plumnin: its file (`dsp1b.rom`, `dsp1.rom`, `dsp2.rom`, `dsp3.rom`, `dsp4.rom`,
  `st010.rom` or `st011.rom`, as other emulators name them, in either byte order) goes
  next to the game, in a `firmware` folder next to it, or in the `firmware` folder beside
  `plumnin.ini`; without it the game says which file it needs. The Super FX (Star Fox,
  Yoshi's Island, Doom...): the GSU with its cache, its buffered ROM and RAM accesses,
  PLOT and RPIX in 2, 4 and 8 bits per pixel, its timing at 10.7 and 21.4 MHz and its
  interrupt; it passes krom's test of each instruction and draws his PLOT demos as his
  screenshots. The SA-1 (Super Mario RPG, the Kirbys...): its 65C816 at 10.7 MHz in
  step with the console's, waiting when both want the same memory, I-RAM and BW-RAM
  (also as a 2 or 4 bits a pixel bitmap) with their write protections, the ROM in 1 MB
  windows, its multiplier, divider and sum of products, the bit field reader, its
  timers, the interrupts and vectors both ways, and its DMA with both character
  conversions. The S-DD1 (Star Ocean, Street Fighter Alpha 2): its ROM banks and the
  decompression of graphics as a DMA reads them. The OBC1 (Metal Combat): its sprite
  table in the cartridge's RAM. The Cx4 (Mega Man X2 and X3): Hitachi's HG51B169 runs
  the game's program from its two caches, with its multiplier, data RAM, DMA and
  interrupt; its table of constants is a firmware file (`cx4.rom`), looked for like the
  DSPs'. The SPC7110 (Far East of Eden Zero, Momotarou Dentetsu Happy, Super Power
  League 4): its banks of the data ROM, the data port, the multiplier and divider, and
  the decompressor of 1, 2 and 4 bpp tiles (not Far East of Eden Zero's clock). Games
  with the other chips (the ST018, the Super Game Boy) load with a warning
  that they may not work: those are not emulated yet, nor the interlaced picture
  (drawn as one field).

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

The plan, in order:

1. The base. (done)
2. Game Boy and Game Boy Color. (done)
3. NES. (done)
4. Super Nintendo. (done; of the cartridges' chips, the NEC DSPs, the Super FX, the SA-1, the S-DD1, the OBC1, the Cx4 and the SPC7110)

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
  answers, on the test pattern; and the Game Boy and the NES on small programs the test
  writes itself (their picture and layers, the pads, the sound, savestates, cheats, the
  battery saves, and PAL on the NES), and the same on a small SNES program; and a
  small program of its own for the NEC DSP, which a SNES program talks to on the bus,
  one for the Super FX (its program, its registers and its interrupt) and one for the
  SA-1 (started by the game, its multiplier, I-RAM, and its interrupt through SIV); the
  S-DD1's decompression through a DMA, against a separate transcription of the
  algorithm, the OBC1's sprite table, a small Cx4 program run from the ROM, and the SPC7110's units, its decompressor
  against a separate transcription too.
- `frontend_tests`: the settings files, each game's own options and the crash reports.
- `headless`: `plumnin-headless` end to end: hashes, savestates, input scripts, recordings
  and netplay between two processes.
- `i18n`: every text in the interface has its translation (`tools/i18n.py`).
- `gb_test_roms`: the Game Boy's test ROMs (blargg, mooneye, dmg-acid2, cgb-acid2), which
  are not part of plumnin. Unpack a release of
  [game-boy-test-roms](https://github.com/c-sp/game-boy-test-roms/releases) and configure
  with `-DGB_TEST_ROMS=path/to/it`. `gb-test-roms --run ROM --png out.png` runs one to look
  at it.
- `nes_test_roms`: the NES's test ROMs, also not part of plumnin. Clone
  [nes-test-roms](https://github.com/christopherpow/nes-test-roms) and configure with
  `-DNES_TEST_ROMS=path/to/it`. `nes-test-roms --run ROM [--pal] --png out.png` runs one
  and prints what it said.
- `snes_test_roms`: krom's SNES test ROMs (each instruction of the processor and of the
  Super FX, the cartridge's banks, the Super FX's PLOT demos against their screenshots,
  a tune checked note by note). Clone
  [PeterLemon/SNES](https://github.com/PeterLemon/SNES) and configure with
  `-DSNES_TEST_ROMS=path/to/it`. `snes-test-roms --run ROM --png out.png` runs one.
- `snes_cpu_tests` and `snes_spc_tests`: [SingleStepTests](https://github.com/SingleStepTests)'
  tests of the 65C816 and of the SPC700, one instruction from a known state each.
  Configure with `-DSNES_CPU_TESTS=path/to/65816/v1` and `-DSNES_SPC_TESTS=path/to/spc700/v1`;
  the 65816's files are large, and the start of each one is enough (CI takes the first
  150 KB). `snes-single-step 65816|spc700 FOLDER --cycles` also compares each bus cycle.

```sh
./build/plumnin-headless --test-rom --system gb --frames 300 --do "60-120 right a" --png out.png
./build/plumnin-headless --help
```

## License

MIT (see [LICENSE](LICENSE)). plumnin does not include any game or BIOS. See
[PRIVACY.md](PRIVACY.md) for what it connects to.
