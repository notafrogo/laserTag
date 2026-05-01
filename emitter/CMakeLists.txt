# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Set the overlay file BEFORE finding Zephyr
set(DTC_OVERLAY_FILE app.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(emitter)

# Add the common folder
add_subdirectory(../common common)

target_sources(app PRIVATE src/main.c)

# Link the app to the newly created zephyr_library in common
target_link_libraries(app PUBLIC common)
target_compile_definitions(app PRIVATE LASERTAG_IS_EMITTER)
target_compile_definitions(common PUBLIC LASERTAG_IS_EMITTER)