cmake_minimum_required(VERSION 3.13)
project(armNES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(ZLIB REQUIRED)

set(CORE_SOURCES
	src/apu.c
	src/boards/asic.c
	src/boards/bandai.c
	src/boards/basic.c
	src/boards/discrete.c
	src/boards/fds.c
	src/boards/jy.c
	src/boards/list.c
	src/boards/mmc3.c
	src/boards/mmc5.c
	src/boards/multicart.c
	src/boards/nsf.c
	src/boards/rare.c
	src/boards/sachen.c
	src/sound/emu2413.c
	src/bus.c
	src/cart.c
	src/cheat.c
	src/cpu.c
	src/input.c
	src/nes.c
	src/ppu.c
	src/ppu-fast.c
	src/state.c
	src/arcade.c
)
# FCEUmm's board files, for the mappers without a board of armNES's own
file(GLOB FCEU_BOARDS src/fceu/boards/*.c)
set_source_files_properties(${FCEU_BOARDS} PROPERTIES COMPILE_OPTIONS "-w")
add_library(armnes STATIC ${CORE_SOURCES} src/fceu/bridge.c ${FCEU_BOARDS})
target_include_directories(armnes PUBLIC src src/boards)
target_include_directories(armnes PRIVATE src/fceu src/fceu/boards shared/libretro)
target_link_libraries(armnes PUBLIC ZLIB::ZLIB m)
target_compile_options(armnes PRIVATE -O3 -falign-functions=64)
set_source_files_properties(src/sound/emu2413.c PROPERTIES COMPILE_OPTIONS "-w")
# The core's parts call each other every master clock: let the compiler inline across its files
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo)
if(ipo)
	set_property(TARGET armnes PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Headless runner for test ROMs and library sweeps
add_executable(nesrun tests/nesrun.c)
target_link_libraries(nesrun armnes m)

# Checks the PPU's catch-up and fast paths against clocking it every master clock
add_executable(selfdiff tests/selfdiff.c)
target_link_libraries(selfdiff armnes)

# The libretro core, with the Armulators pieces it uses (libretro.h, core options) copied into shared/
# (tools/check-shared.sh compares them with the submodule)
add_library(armnes-shared STATIC shared/libretro/options.c)
target_include_directories(armnes-shared PUBLIC shared/libretro)
set_target_properties(armnes-shared PROPERTIES POSITION_INDEPENDENT_CODE ON)
# RetroArch's archive reading (libretro/deps), for games in a .zip or .7z, as armGBA has it
file(GLOB_RECURSE ARCHIVE_SOURCES libretro/deps/libretro-common/*.c libretro/deps/7zip/*.c)
add_library(retro-archive STATIC ${ARCHIVE_SOURCES})
target_include_directories(retro-archive PUBLIC libretro/deps/libretro-common/include PRIVATE libretro/deps)
target_compile_definitions(retro-archive PRIVATE HAVE_ZLIB HAVE_7ZIP)
target_compile_options(retro-archive PRIVATE -w -O2)
set_target_properties(retro-archive PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(armnes_libretro SHARED libretro/libretro.c libretro/composite.c)
if(ANDROID)
	set_target_properties(armnes_libretro PROPERTIES OUTPUT_NAME armnes_libretro_android)
endif()
target_link_libraries(armnes_libretro armnes armnes-shared retro-archive ZLIB::ZLIB m)
set_target_properties(armnes_libretro PROPERTIES PREFIX "")
target_link_options(armnes_libretro PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/shared/libretro/link.T -Wl,--no-undefined)

# armnes_arcade, the same core for RetroArch's arcade folders: it takes .zip and .7z unopened, so MAME's
# VS. System and PlayChoice-10 sets load (armnes leaves archives to RetroArch, for achievements)
add_library(armnes_arcade_libretro SHARED libretro/libretro.c libretro/composite.c)
target_compile_definitions(armnes_arcade_libretro PRIVATE ARMNES_ARCADE)
if(ANDROID)
	set_target_properties(armnes_arcade_libretro PROPERTIES OUTPUT_NAME armnes_arcade_libretro_android)
endif()
target_link_libraries(armnes_arcade_libretro armnes armnes-shared retro-archive ZLIB::ZLIB m)
set_target_properties(armnes_arcade_libretro PROPERTIES PREFIX "")
target_link_options(armnes_arcade_libretro PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/shared/libretro/link.T -Wl,--no-undefined)

# armnes, the standalone app: Armulators' (armulators/app, the only part that needs the submodule) with
# the core linked in, named by app.cmake
option(ARMNES_APP "Build armnes, the standalone SDL2 app" OFF)
if(ARMNES_APP)
	add_subdirectory(armulators)
	armulators_app(armnes-app DESCRIPTOR ${CMAKE_CURRENT_SOURCE_DIR}/app.cmake
		SOURCES libretro/libretro.c libretro/composite.c app/hooks.c INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/libretro
		LIBS armnes armulators-libretro)
endif()

# Drives the libretro core like a frontend and checks save states replay exactly
add_executable(coretest tests/coretest.c)
target_include_directories(coretest PRIVATE shared/libretro)
target_link_libraries(coretest ZLIB::ZLIB dl)
