# Set the minimum version of CMake that can be used
cmake_minimum_required(VERSION 3.20)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
set(CMAKE_C_COMPILER_FORCED TRUE)
set(CMAKE_CXX_COMPILER_FORCED TRUE)

# Set target definition variables required by the toolchain file
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/target.cmake)

# Set the toolchain file for cross-compilation
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake)

# Project definition
project(mpu6050)

# Enable CMake support for ASM and C/CPP languages
enable_language(C CXX ASM)

set(PROJECT_NAME mpu6050)

# Create an executable object type
add_executable(${PROJECT_NAME})

# Include additional CMake files for flags, components, and files
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/flags.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/components.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/files.cmake)

# Add sources to executable/library
target_sources(${PROJECT_NAME} PRIVATE
  # User defined sources
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/timebase.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/i2c.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/mpu6050.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/spi.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/st7789.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/gfx.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/font_data.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/ui.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/clock.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/usb.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/usb_cdc.c
)

# Add include directories
target_include_directories(${PROJECT_NAME} PRIVATE
  # User defined include directories
)

# Compiler options
target_compile_options(${PROJECT_NAME} PRIVATE
  # User defined compiler options
)

# Add additional defined symbols
target_compile_definitions(${PROJECT_NAME} PRIVATE
  # Add additional defined symbols here
)

# Linker options
target_link_options(${PROJECT_NAME} PRIVATE 
  # User defined linker options
)

# Add additional linker search directories
target_link_directories(${PROJECT_NAME} PRIVATE
  # Add additional linker search directories here
)

# Add additional linker libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
  # Add additional linker libraries here
  m   # atan2f/sqrtf for the TFT tilt readout (the -lm in flags.cmake comes before the objects)
)

# ---------------------------------------------------------------------------
# USB DFU bootloader: a second, independent image in the first 8 KB of flash
# (bootloader/bootloader.ld). It shares the USB core, the clock set-up and the
# startup file with the application, and is always optimised for size so it
# fits in 8 KB even in a Debug build.
# ---------------------------------------------------------------------------
add_executable(bootloader
  ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/dfu_bootloader.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/usb.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/clock.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/timebase.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/startup_stm32f103xx.S
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/syscall.c   # newlib stubs (unused, avoid link warnings)
  ${CMAKE_CURRENT_SOURCE_DIR}/Src/sysmem.c
)
set_target_properties(bootloader PROPERTIES SUFFIX ".elf")
target_include_directories(bootloader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Inc)
target_compile_options(bootloader PRIVATE
  ${CPU_FLAGS}
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Os -g3 -Wall -Wextra -Wpedantic -Wno-unused-parameter>
  $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>
)
target_link_options(bootloader PRIVATE
  ${CPU_FLAGS}
  -T${CMAKE_CURRENT_SOURCE_DIR}/bootloader/bootloader.ld
  --specs=nano.specs --specs=nosys.specs
  -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/bootloader.map
  -Wl,--print-memory-usage
)
set_target_properties(bootloader PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/bootloader.ld)
