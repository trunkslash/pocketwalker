cmake_minimum_required(VERSION 3.22.1)

project(pocketwalker_android)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core
    ${CMAKE_CURRENT_BINARY_DIR}/pocketwalker_core
)

add_library(
    pocketwalker_jni
    SHARED
    native-lib.cpp
)

find_library(log-lib log)

target_include_directories(
    pocketwalker_jni
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../
)

target_link_libraries(
    pocketwalker_jni
    pocketwalker_core
    ${log-lib}
)

# Optimize the native emulator even in Android debug builds.
# This is especially important on lower-power Wear OS devices.
target_compile_options(pocketwalker_core PRIVATE -O2)
target_compile_options(pocketwalker_jni PRIVATE -O2)
