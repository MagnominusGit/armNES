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
	src/boards/sachen.c
	src/sound/emu2413.c
	src/bus.c
	src/cart.c
	src/cpu.c
	src/nes.c
	src/ppu.c
	src/ppu-fast.c
	src/state.c
)
# FCEUmm's board files, for the mappers without a board of armNES's own
file(GLOB FCEU_BOARDS src/fceu/boards/*.c)
set_source_files_properties(${FCEU_BOARDS} PROPERTIES COMPILE_OPTIONS "-w")
add_library(armnes STATIC ${CORE_SOURCES} src/fceu/bridge.c ${FCEU_BOARDS})
target_include_directories(armnes PUBLIC src src/boards)
target_include_directories(armnes PRIVATE src/fceu src/fceu/boards armulators/libretro)
target_link_libraries(armnes PUBLIC ZLIB::ZLIB m)
target_compile_options(armnes PRIVATE -O3)
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

# The libretro core, with Armulators' libretro pieces (libretro.h, core options)
add_subdirectory(armulators)
add_library(armnes_libretro SHARED libretro/libretro.c)
if(ANDROID)
	set_target_properties(armnes_libretro PROPERTIES OUTPUT_NAME armnes_libretro_android)
endif()
target_link_libraries(armnes_libretro armnes armulators-libretro m)
set_target_properties(armnes_libretro PROPERTIES PREFIX "")
target_link_options(armnes_libretro PRIVATE -Wl,--version-script=${ARMULATORS_LINK_SCRIPT} -Wl,--no-undefined)

# Drives the libretro core like a frontend and checks save states replay exactly
add_executable(coretest tests/coretest.c)
target_include_directories(coretest PRIVATE armulators/libretro)
target_link_libraries(coretest ZLIB::ZLIB dl)
