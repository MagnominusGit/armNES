# Archive reading

Games in `.zip` and `.7z` files are read by the core itself, with RetroArch's own archive code, as
armGBA does: RetroArch mustn't extract them, because a MAME set has no game file inside to extract.

- `libretro-common/`: the files RetroArch's archive reading needs, from RetroArch
  bdba046fa6766380bc2457532f38e589df769aaf (v1.22.2 and fixes, the version ROCKNIX ships). MIT
  license, stated in each file. Its `libretro.h` is used only by this code.
- `7zip/`: the LZMA SDK's decoder, from RetroArch's `deps/7zip` at the same commit. Public domain.

The files are unchanged, copied from armGBA's `libretro/deps`. Zip support uses the system's zlib.
