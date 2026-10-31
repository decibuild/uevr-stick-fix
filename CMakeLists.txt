cmake_minimum_required(VERSION 3.15)
project(uevr_stick_fix CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT, so the DLL needs nothing else installed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(stick_fix SHARED src/stick_fix.cpp)
target_include_directories(stick_fix PRIVATE include)
