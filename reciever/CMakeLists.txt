cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(reciever)

# Add the common folder
add_subdirectory(../common common)

target_sources(app PRIVATE src/main.c)

# Link the app to the newly created zephyr_library in common
target_link_libraries(app PUBLIC common)
target_compile_definitions(app PRIVATE LASERTAG_IS_RECEIVER)
target_compile_definitions(common PUBLIC LASERTAG_IS_RECEIVER)
