# What the standalone app (Armulators' app/) is for this core: its names, Android package, look and the
# games it opens. CMake writes them into app-info.h for the C code.
set(APP_ID armnes)
set(APP_NAME armNES)
set(APP_WORDMARK ARMNES)
set(APP_ACCENT Red)
set(APP_JAVA_PACKAGE org.armnes.app)
set(APP_JAVA_CLASS Armnes)
# Its version, and how it's built: the CMake option that turns it on, and its target.
set(APP_VERSION 0.1)
set(APP_BUILD_OPTION ARMNES_APP)
set(APP_BUILD_TARGET armnes-app)
# For the desktop: what it is, and the files it opens.
set(APP_COMMENT "NES and Famicom Disk System emulator")
set(APP_MIME application/x-nes-rom application/x-fds-disk application/zip)
# The console's screen: the window is a multiple of it before a game says its own.
set(APP_WINDOW 256 240)
# The game list's files (archives too), and what's looked for inside an archive.
set(APP_EXTENSIONS nes fds unf unif nsf zip 7z)
set(APP_ARCHIVED nes fds unf unif nsf)
# The systems: each one's key (its folder setting is <key>_folder), name, and the folder names it's
# found by in the Games folder (its key and these, any case).
set(APP_SYSTEMS nes fds)
set(APP_SYSTEM_nes "NES" famicom "nintendo entertainment system")
set(APP_SYSTEM_fds "Famicom Disk System" famicomdisksystem "famicom disk system")
# The keyboard's default keys, as SDL names them (NONE for none), in libretro's order: B, Y, Select,
# Start, Up, Down, Left, Right, A, X, L, R, L2, R2, L3, R3. APP_BUTTON_NAMES names them in the menus,
# - for one the core doesn't use, and APP_BUTTON_ORDER lists them by libretro's numbers.
# Z and X are B and A, A and S their turbo buttons.
set(APP_KEYS Z A BACKSPACE RETURN UP DOWN LEFT RIGHT X S Q W 1 2 NONE NONE)
set(APP_BUTTON_NAMES B "Turbo B" Select Start "D-Pad Up" "D-Pad Down" "D-Pad Left" "D-Pad Right" A "Turbo A" L R L2 R2 - -)
# A, B, their turbo buttons, Select, Start, the D-pad, then L, R, L2 and R2.
set(APP_BUTTON_ORDER 8 0 9 1 2 3 4 5 6 7 10 11 12 13 14 15)
# Help texts that name the console's buttons and what the app draws over its picture.
set(APP_HELP_SWAP "Off: A is the right button, as on an NES controller. On: A and B swap, and X and Y.")
