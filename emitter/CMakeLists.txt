# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE app.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(emitter)

add_subdirectory(../common common)

target_sources(app PRIVATE
    src/main.c
    src/ble_phone.c
    src/ble_vest.c
    src/game_state.c
    src/mesh.c
)

target_link_libraries(app PUBLIC common)
target_include_directories(app PRIVATE src)
