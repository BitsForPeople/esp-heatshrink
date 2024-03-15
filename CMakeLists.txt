# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED False)

idf_component_register(
    SRCS "heatshrink_encoder.cpp" "heatshrink_decoder.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "private"
)