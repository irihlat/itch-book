# Build: cmake -S . -B build && cmake --build build
# Run:   ./build/census data/20190730.BX_ITCH_50   (or ./build/orders ...)
cmake_minimum_required(VERSION 3.20)
project(itch_book CXX)  # CXX = C++

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # lets VS Code find our includes

# Optimised by default; pass -DCMAKE_BUILD_TYPE=Debug for debugger stepping.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)  # warnings on

add_executable(census src/census.cpp)
target_include_directories(census PRIVATE include)  # finds "itch/endian.hpp"

add_executable(orders src/orders.cpp)
target_include_directories(orders PRIVATE include)
