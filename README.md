# armNES

An NES and Famicom libretro core, with its own standalone app, for ARM and x86 handhelds.

armNES aims to be accurate in everything a game shows and plays, and fast enough for low-power
chips. Its PPU and APU timing follows TriCNES, and most of its cartridge boards beyond the common
ones come from FCEUmm. See Credits for the full list.

It has three PPU models. Fast, the default, leaves the most headroom. Balanced and Exact pass every
one of AccuracyCoin's 147 tests: Balanced at under twice Fast's cost, Exact cycle for cycle, for
desktops.

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
  In RetroArch, use the `armnes_arcade` core for them (see Building).
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

The menu shows only the options that do something for the game loaded: the VS. System category only
for arcade games, Disk System Fast Loading only for disks, Cart Jumper only for carts that have one,
the cart sound options only for carts with their own sound chip, and the Zapper options only with a
Zapper plugged in (the motion ones only with Zapper Motion on).

| Category | Option | Values (default first) |
| --- | --- | --- |
| System | Region | Auto (from the header), NTSC, PAL, Dendy |
| | PPU Accuracy (applies when a game is loaded) | Fast, Balanced, Exact |
| | Overclock | Off, 2x, 3x or 4x between frames, 2x or 3x after the picture |
| | Disk System Fast Loading | Off, On (up to 16 times as fast while the drive reads or writes) |
| | Cart Jumper (restarts the game) | Auto (the setting known for the dump), 0 to 23 |
| Video | Palette (without the filter, which makes its own colors) | armNES NTSC (decoded from the video signal), 23 measured or tuned palettes from FCEUmm's set, Custom |
| | NTSC/PAL Filter | Off, Composite, S-Video (not used on arcade boards, whose PPU outputs RGB) |
| | Scanlines (with the filter) | Off, Light, Medium, Dark |
| | Sprite Limit | 8 per line, No limit (games still see the console's limit) |
| | Aspect Ratio | 4:3, Console pixels (8:7), Square pixels |
| | Crop Top and Bottom | 8, then 0, 4, 12, 16 pixels |
| | Crop Sides | 0, 4, 8, 12, 16 pixels |
| Audio | Pulse 1, Pulse 2, Triangle, Noise, DMC (Samples) and Cart Sound Volume | 100%, 0% to 90% in tenths |
| | The same channels' Position | Center, Left to Right in quarters |
| Input | Turbo Speed | 15 per second, Off, 30, 20, 10, 7.5 per second |
| | Allow Opposing Directions | Off, On |
| | Left Stick as D-Pad | On, Off |
| | 4 Players | Auto (from an NES 2.0 header), Off, Four Score, Famicom adapter |
| | Famicom Expansion Port | Auto (from an NES 2.0 header), None, Paddle, Power Pad, Keyboard |
| Zapper | Zapper Input | Right stick (moves), Right stick (points), Light gun, Mouse; touch always aims and fires |
| | Zapper Sensitivity | 100%, 25% to 300% |
| | Zapper Stick Invert | Off, Horizontal, Vertical, Both |
| | Zapper Motion | Off, Gyroscope, Accelerometer |
| | Zapper Motion Range | Whole screen, Fine (a quarter of the screen either way) |
| | Zapper Motion Sensitivity | 100% (about 20° for the whole range), 25% to 300% |
| | Zapper Motion Invert | Off, Horizontal, Vertical, Both |
| | Zapper Motion Sensor Rotation | 0°, 90°, 180°, 270° |
| VS. System | PPU | Auto (from an NES 2.0 header, or the RP2C03), RP2C03, the four RP2C04s, the five RC2C05s |
| | Swap Controllers | Auto (from an NES 2.0 header), Off, On |
| | DIP Switch 1 to 8 | Off, On |
| | DualSystem Screens | Both, Main board, Second board |
| | Second Board DIP Switch 1 to 8 (DualSystem) | Off, On |

The NTSC/PAL filter decodes the console's video signal as a TV of the time did: Composite with its
color fringes, blended dithering and dot crawl, or the cleaner S-Video. Its picture has two rows to a
line, so Scanlines can darken every other row; integer scaling or a smooth filter in the frontend
shows them evenly. Custom palettes go in the system folder as `nes.pal` or `custom.pal` (64 colors,
or 512 with the emphasis combinations).

## Building

    cmake -S . -B build
    cmake --build build --target armnes_libretro

This makes `armnes_libretro.so`; `libretro/armnes_libretro.info` goes in RetroArch's info directory.
The `armnes_arcade_libretro` target builds the same core for RetroArch's arcade folders: `armnes`
lets RetroArch unpack archives, so achievements recognize zipped games, while `armnes_arcade` takes
`.zip` and `.7z` files unopened, so MAME's VS. System and PlayChoice-10 sets load. Each release
carries both cores for x86-64 and 64-bit ARM Linux, with the AppImages and the APK.

For RetroArch on Android, build with the NDK. The cores are then named `armnes_libretro_android.so`
and `armnes_arcade_libretro_android.so`:

    cmake -S . -B build-android -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
    cmake --build build-android --target armnes_libretro

The test tools are the `nesrun`, `coretest` and `selfdiff` targets (see Diagnostics).

## Standalone app

`armnes` runs games in a window without RetroArch. It is Armulators' app (the `armulators`
submodule), which armGBA and armN64 share; armNES gives it its names and systems (`app.cmake`) and
its Disk System and achievement hooks (`app/`):

    git submodule update --init
    cmake -S . -B build -DARMNES_APP=ON
    cmake --build build --target armnes-app

`armulators/package/appimage/build.sh`, then `package.sh` beside it, make AppImages for x86-64 and
64-bit ARM Linux in a Debian 11 container (Docker), with SDL2 built in.
`armulators/package/android/build.sh` makes the Android APK.

### Running and controls

Run `armnes game.nes` (or a `.fds`, `.unf`, `.nsf`, `.zip` or `.7z` file), or drop a game on the
window. Esc, a controller's Hotkey + X or a right-click opens the menu, where all of these can be
changed.

| Keyboard | Does |
| --- | --- |
| Arrow keys | D-pad |
| X, Z | A, B |
| S, A | Turbo A, turbo B |
| Enter, Backspace | Start, Select |
| Q, W | L, R |
| 1, 2, 3, 4 | L2, R2, L3, R3 |
| Tab (held) | Fast-forward |
| R | Rewind |
| P | Pause |
| F2 | Restart |
| F5, F7 | Save, load a state |
| F6, F8 | Change the state slot |
| F3 | Show the frame rate |
| F9 | Start or stop recording |
| F11 | Full screen |
| F12 | Screenshot |

Controllers work too, one per player, with ROCKNIX's hotkeys held with the Hotkey (Home, or Select
on a controller without one): Start twice quits, A pauses, R and L save and load a state, R2 turns
fast-forward on or off, L2 rewinds, X opens the menu, Y shows the frame rate and B takes a
screenshot. A touch screen aims and fires the Zapper.

### Menu

The menu has four tabs, which L and R move between: General (recent games, Load Game, the app's
settings and Quit), Game (state slots with pictures, cheats, Disk Sides for disk games, Restart,
screenshots, recording and Close Game), Core (the core's options, as above) and Extras (netplay and
RetroAchievements). In the game list, L and R page through it, faster the longer they're held, and
L2 and R2 jump by letter.

### Netplay

The app's netplay connects armnes to armnes. Every device runs the game, kept in step, with each
controller on a device a player, up to four. The host's game loads from the other devices' folders
by its file name. Achievements need the system's libcurl, and recording needs its ffmpeg.

### Files

The app's settings are in `~/.config/armnes/frontend.cfg`, the core's options in
`~/.config/armnes/armnes.cfg` and the recent games in `~/.config/armnes/recent.txt`. `disksys.rom`
goes in `~/.config/armnes/system`, and saves, states, screenshots and cheats in
`~/.local/share/armnes`. On ROCKNIX, the app follows ROCKNIX's other standalone emulators: BIOS
files come from `roms/bios`, saves sit beside the games, and states and screenshots go in
`roms/savestates` and `roms/screenshots`. Saves and Folders in the app's settings changes each of
these.

## Accuracy

What games show and play comes first; test ROMs that probe corners no game reaches come second.

- The PPU draws each pixel at the cycle it would on the console, so mid-frame effects, split screens
  and sprite 0 hits land where they should.
  - **Exact** models the PPU half a dot at a time and the CPU every master clock, after TriCNES.
    It passes all 147 of AccuracyCoin's tests, and it's the reference the other two are checked
    against. It is many times slower: for desktops.
  - **Balanced** finds and fetches sprites dot by dot as the hardware does, with the console's
    address latch, read buffer and OAM corruption quirks. It passes all 147 too, at under twice
    Fast's cost.
  - **Fast**, the default, draws whole tiles and lines where nothing can change them, and passes
    132.
- The APU passes blargg's `apu_test` suite, including its DMC tests, and mixes its channels as the
  console's nonlinear mixer does. The Disk System's, VRC6's, VRC7's, MMC5's, Namco 163's and Sunsoft
  5B's sound chips are emulated.
- Save states are deterministic, so netplay, rewind and run-ahead stay in step. RetroAchievements,
  cheats (Game Genie and raw codes) and memory maps are supported.

## Testing

- **AccuracyCoin:** 147 of 147 with Exact and Balanced, 132 with Fast.
- **blargg's tests:** `apu_test` passes, with its DMC tests.
- **Speed changes:** every change made for speed is run across my games before and after, with a
  fixed button script, and has to leave the picture, the sound and the console's RAM unchanged in
  Fast and Balanced. Fast-forward's skipped frames have to leave the frame shown unchanged too.
- **Accuracy changes:** a change that alters Balanced's picture is compared with Exact's in every
  game whose picture changed, at several points; most have to come closer to it, and those that
  don't are looked into one by one.
- **Save states:** `coretest` checks that a state taken and loaded again replays the same picture
  and sound.

## Performance

Frames per second on a Raspberry Pi 5 (Cortex-A76, 2.4 GHz), one core, unthrottled, every frame
drawn, through the libretro API with the same harness for every core. Full speed is about 60. The
games are four commercial ones: a scrolling platformer on the plain NROM board, two MMC3 games with
raster splits and many sprites, and an MMC1 action game. AccuracyCoin scores are each core's own.

| Core | NROM platformer | MMC3 game 1 | MMC3 game 2 | MMC1 game | AccuracyCoin |
| --- | --- | --- | --- | --- | --- |
| armNES, Fast | 2330 | 1770 | 1519 | 2139 | 132 |
| armNES, Balanced | 1301 | 1095 | 1004 | 1282 | 147 |
| armNES, Exact | 146 | 139 | 140 | 145 | 147 |
| Nestopia (built from its source, October 2026) | 1264 | 919 | 906 | 1159 | 147 |
| Nestopia 1.53, as Batocera ships it | 1546 | 1157 | 1126 | 1518 | doesn't finish |
| FCEUmm, as Batocera ships it | 2424 | 2414 | 2322 | 2389 | 84 |
| Mesen 0.9.9, as Batocera ships it | 411 | 384 | 375 | 354 | 109 |

Balanced runs as fast as the current Nestopia or faster at the same score, and Fast runs 1.7 to 1.9
times as fast. FCEUmm is faster still, timing the CPU and PPU less exactly. The NTSC/PAL filter
halves the speed, which on the Pi 5 still leaves over ten times what a game needs.

## How it works

**Three PPUs.** Exact (`src/ppu.c`) steps the PPU every master clock with half-dot
latches. Fast and Balanced (`src/ppu-fast.c`) are caught up lazily: the PPU runs only when the CPU
touches it, or when something it does could reach the CPU (an NMI, a sprite 0 hit it's polling for,
a board's IRQ).

**Whole tiles at once.** Where nothing can intervene, Fast and Balanced draw eight pixels of a tile
in one step: the background from its shift registers, and the sprite slots' pixels composited at
once with NEON or SSE. Balanced still finds and fetches sprites as the hardware does, but runs a
tile's eight dots of that work together, and a stretch of sprite evaluation in one pass, skipping
the sprites not on the line.

**Quiet cycles.** The CPU knows how many cycles can pass before the PPU, the APU or the board could
change anything it sees, and runs that many with only its memory accesses, counting their time when
the budget runs out. A loop that does nothing but wait on `$2002` skips its passes to the moment the
flag can change. An MMC3's IRQ counter is bounded by where its A12 line can next rise.

**Sound.** The APU advances its channels in bulk to the next point where a channel's output can
change, and each sample is the average level since the last one.

**Frames nobody sees.** On fast-forward and run-ahead, frames the frontend won't show skip their
pixels unless a Zapper or a sprite 0 hit needs them.

## Diagnostics

The test tools run a game headless. `nesrun <rom> [frames]` prints a test ROM's result, or a hash of
the last frame; `coretest <core> <rom>` drives any libretro core and checks its save states;
`selfdiff <rom> [frames]` runs Exact and Fast side by side and reports the first frame they differ.
`nesrun` takes these environment variables:

| Variable | Effect |
| --- | --- |
| `NESRUN_BALANCED=1`, `NESRUN_EXACT=1` | The Balanced or Exact PPU |
| `NESRUN_PRESS="frame:mask[:frames],..."` | Hold controller 1's buttons (bit 0 A to bit 7 Right) |
| `NESRUN_KEYS="frame:key[:frames],..."` | Hold Famicom keyboard keys, by libretro key code |
| `NESRUN_RESET=frame` | Press reset then |
| `NESRUN_DISK="frame:side,..."` | Put a disk side in the drive from then, -1 for none |
| `NESRUN_BIOS=<file>` | The Disk System BIOS |
| `NESRUN_CHEAT=<codes>`, `NESRUN_JUMPER=n` | Cheats, and the cart's jumper setting |
| `NESRUN_OVERCLOCK=lines`, `NESRUN_POSTRENDER=1`, `NESRUN_SPRITES=1` | Overclock, after the picture; no sprite limit |
| `NESRUN_SKIP=1` | Every frame but the last runs as one nobody sees |
| `NESRUN_WAV=<file>`, `NESRUN_PPM=<file>`, `NESRUN_RAM=<file>` | Write the sound, the last frame, or the console's RAM |
| `NESRUN_TEXT=1`, `NESRUN_INFO=1` | Print the name table as text; print the header as read and stop |

`coretest`'s own variables are listed at the top of `tests/coretest.c`.

## Known gaps

- The fast PPU misses 15 of AccuracyCoin's 147 tests, on behavior no game is known to depend on.
- Three of blargg's APU reset tests fail: what a soft reset leaves in the frame counter and length
  counters.
- Overclock gives games extra CPU time each frame; a few games misbehave with it.
- The app's netplay connects only to another armnes, not to RetroArch.

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
- **stb_truetype** (Sean Barrett, public domain or MIT): the app's text, in Armulators' `app/`.
- **Inter** (The Inter Project Authors) and **Chakra Petch** (The Chakra Petch Project Authors),
  OFL-1.1: the app's and the NSF player's fonts.

Read for how the hardware behaves, without code taken: **Mesen** (the VS. System boards and their
protection, DualSystem wiring, and several unlicensed boards), the **NESdev Wiki**, and **Nestopia**.

Test ROMs: AccuracyCoin, blargg's NES tests and the nes-test-roms collection.

## AI assistance

armNES was written with heavy use of an AI coding assistant, Anthropic's Claude (through Claude
Code). Most of the code, comments and documentation were drafted by it at Magnominus's direction. He
chose what to build and decides what goes in. Each change was tested on his PCs and handhelds first,
and the accuracy and speed claims here come from those tests, listed above, not from the
assistant's word.

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
