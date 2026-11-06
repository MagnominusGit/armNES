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
	src/boards.c
	src/bus.c
	src/cart.c
	src/cpu.c
	src/nes.c
	src/ppu.c
)
add_library(armnes STATIC ${CORE_SOURCES})
target_include_directories(armnes PUBLIC src)
target_link_libraries(armnes PUBLIC ZLIB::ZLIB)
target_compile_options(armnes PRIVATE -O3)

# Headless runner for test ROMs and library sweeps
add_executable(nesrun tests/nesrun.c)
target_link_libraries(nesrun armnes m)
