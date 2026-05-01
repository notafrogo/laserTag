cmake_minimum_required(VERSION 3.20.0)

add_library(common STATIC src/ble_gatt.c src/ir_handler.c)
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(common PUBLIC zephyr_interface)
