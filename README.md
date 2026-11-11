# armNES

An NES and Famicom libretro core, with its own standalone app, for ARM and x86 handhelds.

armNES aims to be accurate in everything a game shows and plays, and fast enough for low-power
chips. Its PPU and APU timing follows TriCNES, and most of its cartridge boards beyond the common
ones come from FCEUmm. See Credits for the full list.

## Legal

- **Your own games.** armNES comes with no games and doesn't download any. Play only games you own,
  dumped from your own cartridges or bought legally. armNES doesn't support or promote piracy.
- **Nothing is shared in netplay.** Each player needs their own copy of the game, and of the Disk
  System BIOS for disk games.
- **Not Nintendo.** armNES is an independent project, not affiliated with or endorsed by Nintendo.
  Nintendo Entertainment System, Famicom, Famicom Disk System, VS. System and PlayChoice-10 are
  trademarks of Nintendo, and the Disk System BIOS is Nintendo's copyright.

## What it runs

- NES and Famicom games: `.nes` (iNES and NES 2.0) and `.unf`/`.unif`, with hundreds of mappers,
  including unlicensed boards and multicarts. Carts with solder pads or DIP switches that pick their
  menu or language get the Cart Jumper option.
- Famicom Disk System games (`.fds`), with `disksys.rom` in the frontend's system folder. The core
  turns to the side the BIOS asks for; L3 takes the disk out and puts the next side in, as does the
  frontend's disc menu.
- VS. System and PlayChoice-10 arcade games, from MAME's `.zip` or `.7z` sets (a clone finds its
  parent beside it), with each board's PPU colors, protection chips, coins and DIP switches. A
  DualSystem's two boards run together, their screens side by side. The armNES app opens these sets.
  In RetroArch, use the `armnes_arcade` core for them (see below).
- NSF music files, with a small player.
- Games inside `.zip` and `.7z` archives. RetroArch unpacks them, so achievements recognize the game.

## Controls

| Button | What it does |
| --- | --- |
| A, B, Select, Start, D-pad | The controller's own; Select also puts in a coin on VS. System games |
| X, Y | Turbo A, turbo B, at the Turbo Speed option's rate |
| R | Zapper trigger, when it aims with the right stick |
| L | Zapper trigger off the screen, when it aims with the right stick |
| R2 | Fast-forward while held |
| L2 | Zapper fine aim: the further it's pulled, the slower the crosshair moves |
| R3 | Zapper: centers the crosshair; with Zapper Motion, how the device is held becomes the middle |
| L3 | On Disk System games, takes the disk out, then puts the next side in; VS. System service button; on controller 2, the Famicom's microphone; on Datach games, a barcode swipe |
| Left stick | The D-pad too, unless Left Stick as D-Pad is off |

Plug in a Zapper, Arkanoid paddle or Power Pad through the frontend's controller port types. With
Auto, the default, a game whose NES 2.0 header names one gets it, and Controller always plugs in a
pad. The Zapper aims with a light gun or mouse pointer, a mouse-moved crosshair, or the right stick,
and a touch screen aims and fires wherever it's touched. Zapper Motion (off by default) adds the
device's motion on top of the right stick: turning it with the gyroscope or tilting it with the
accelerometer, across the whole screen or for fine aim. Players 3 and 4 use the NES Four Score or
the Famicom's adapter; the Famicom's expansion port also takes the paddle, the Power Pad and the
Famicom keyboard.

## Options

The options come in five groups, and the menu shows only those that do something for the game
loaded: the VS. System group only for arcade games, Disk System Fast Loading only for disks, Cart
Sound only for carts with their own sound chip, and the Zapper options only with a Zapper plugged in.

| Group | Options |
| --- | --- |
| System | Region (Auto, NTSC, PAL, Dendy), PPU Accuracy (Fast, Balanced, Exact), Overclock, Disk System Fast Loading, Cart Jumper |
| Video | Palette, NTSC/PAL Filter (Composite, S-Video), Scanlines, Sprite Limit, Aspect Ratio, Crop Top and Bottom, Crop Sides |
| Audio | Volume and stereo position for each channel, and for the cart's sound chip |
| Input | Turbo Speed, Allow Opposing Directions, Left Stick as D-Pad, 4 Players, Famicom Expansion Port, Zapper Input, Zapper Sensitivity, Zapper Stick Invert, Zapper Motion, Zapper Motion Range, Zapper Motion Sensitivity, Zapper Motion Invert, Zapper Motion Sensor Rotation |
| VS. System | PPU, Swap Controllers, DIP switches, DualSystem Screens |

The NTSC/PAL filter decodes the console's video signal as a TV of the time did: Composite with its
color fringes, blended dithering and dot crawl, or the cleaner S-Video. Its picture has two rows to a
line, so Scanlines can darken every other row; integer scaling or a smooth filter in the frontend
shows them evenly. Custom palettes go in the system folder as `nes.pal` or `custom.pal` (64 colors,
or 512 with the emphasis combinations).

## Accuracy

What games show and play comes first; test ROMs that probe corners no game reaches come second.

- The PPU draws each pixel at the cycle it would on the console, so mid-frame effects, split screens
  and sprite 0 hits land where they should. The cycle-exact PPU (PPU Accuracy: Exact, many times
  slower, for desktops) passes all 147 of AccuracyCoin's tests. Balanced, which finds and fetches
  sprites dot by dot as the hardware does, passes 139; Fast, the default, passes 131. Their pictures
  are checked against the cycle-exact one frame by frame.
- The APU passes blargg's `apu_test` suite, including its DMC tests, and mixes its channels as the
  console's nonlinear mixer does. The Disk System's, VRC6's, VRC7's, MMC5's, Namco 163's and Sunsoft
  5B's sound chips are emulated.
- Save states are deterministic, so netplay, rewind and run-ahead stay in step. RetroAchievements,
  cheats (Game Genie and raw codes) and memory maps are supported.

## Performance

On a Raspberry Pi 5, on one core, armNES runs a set of eleven test games about 4% faster in total
than Nestopia, and nearly four times as fast as Mesen. FCEUmm is faster still: it times the CPU and
PPU less exactly, which is where the difference goes. The NTSC/PAL filter halves the speed, which on
the Pi 5 still leaves over ten times what a game needs.

## Standalone app

`armnes` runs games in a window without RetroArch: the app Armulators provides (`armulators/app`),
with menus, save states, rewind, fast-forward, cheats, netplay between armnes devices and
RetroAchievements. Its AppImages run on x86-64 and 64-bit ARM Linux, and its APK on Android. It
keeps its settings in `~/.config/armnes` and its saves, states, screenshots and cheats in
`~/.local/share/armnes`; `disksys.rom` goes in `~/.config/armnes/system`.

## Building

    cmake -S . -B build
    cmake --build build --target armnes_libretro

This makes `armnes_libretro.so`; `libretro/armnes_libretro.info` goes in RetroArch's info directory.
The `armnes_arcade_libretro` target builds the same core for RetroArch's arcade folders: `armnes`
lets RetroArch unpack archives, so achievements recognize zipped games, while `armnes_arcade` takes
`.zip` and `.7z` files unopened, so MAME's VS. System and PlayChoice-10 sets load. Each release
carries both cores for x86-64 and 64-bit ARM Linux, with the AppImages and the APK.
With `-DARMNES_APP=ON`, the `armnes-app` target builds the standalone app (SDL2 needed).
`armulators/package/appimage/build.sh` and `package.sh` make the AppImages in a Debian 11 container,
and `armulators/package/android/build.sh` makes the APK.

## Known gaps

- The fast PPU misses 16 of AccuracyCoin's 147 tests, on behavior no game is known to depend on.
- Three of blargg's APU reset tests fail: what a soft reset leaves in the frame counter and length
  counters.
- Overclock gives games extra CPU time each frame; a few games misbehave with it.

## Credits

Code taken from other projects, each under its own license (see License):

- **TriCNES** (Chris Siebert, MIT): the PPU model and the timing of the frame counter, DMC, `$4015`
  and the controllers, in `src/ppu.c` and `src/apu.c`.
- **FCEUmm** (Xodnizel, CaH4e3, NewRisingSun, the libretro team and others, GPL-2.0-or-later): the
  cartridge boards in `src/fceu/boards/` and the mapping functions they use, and the palettes in
  `libretro/palettes.inc`.
- **emu2413** (Mitsutaka Okazaki, MIT): VRC7 sound, in `src/sound/`.
- **MAME** (BSD-3-Clause): the chip layouts of the VS. System and PlayChoice-10 sets, in
  `src/arcade-sets.inc`.
- **RetroArch** (MIT, public domain): libretro-common and the LZMA SDK in `libretro/deps/`, and
  rcheevos in Armulators' `deps/`.
- **Chakra Petch** (The Chakra Petch Project Authors, OFL-1.1): the NSF player's font.

Read for how the hardware behaves, without code taken: **Mesen** (the VS. System boards and their
protection, DualSystem wiring, and several unlicensed boards), the **NESdev Wiki**, and **Nestopia**.

Test ROMs: AccuracyCoin, blargg's NES tests and the nes-test-roms collection.

## AI assistance

armNES was written with heavy use of an AI coding assistant, Anthropic's Claude (through Claude
Code). Most of the code, comments and documentation were drafted by it at Magnominus's direction. He
chose what to build and decides what goes in. Each change was tested on his PCs and handhelds first,
and the accuracy and speed claims here come from those tests, not from the assistant's word.

## License

Distributed under the GNU General Public License, version 2 (`LICENSE`), as FCEUmm's boards are.
armNES's own files are GPL-2.0-or-later. Each part keeps its own notice:

| Part | License |
| --- | --- |
| `src/`, `libretro/`, `tests/`, `app/`, `app.cmake` | GPL-2.0-or-later; `src/ppu.c` and `src/apu.c` follow TriCNES (MIT, `LICENSES/MIT.txt`) |
| `src/fceu/` | GPL-2.0-or-later, from FCEUmm |
| `src/sound/emu2413.*` | MIT (`LICENSES/MIT-emu2413.txt`) |
| `libretro/deps/` | MIT and public domain, as each file states |
| `shared/` | copies of the Armulators files the cores build from, so they build without the submodule (`tools/check-shared.sh` compares them): GPL-2.0-or-later, `libretro.h` MIT |
| `armulators/` | its own notices, needed for the standalone app only: GPL-2.0-or-later, with the fonts under the SIL Open Font License 1.1 and its `deps/` MIT and public domain |
