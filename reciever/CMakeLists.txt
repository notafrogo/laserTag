cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE app.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(reciever)

add_subdirectory(../common common)

target_sources(app PRIVATE
    src/main.c
    src/vest_ble.c
)

target_link_libraries(app PUBLIC common)
target_include_directories(app PRIVATE src)
