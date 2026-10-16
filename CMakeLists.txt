cmake_minimum_required(VERSION 3.25)
project(fm2 C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "fm2 needs clang-cl (the recompiled code uses clang builtins)")
endif()

add_subdirectory(thirdparty/XenonRecomp EXCLUDE_FROM_ALL)

# Recompiled game code (out/ppc, produced by XenonRecomp from your own
# default.xex; never committed).
set(FM2_PPC_DIR "${CMAKE_SOURCE_DIR}/out/ppc")
file(GLOB FM2_PPC_SOURCES CONFIGURE_DEPENDS "${FM2_PPC_DIR}/*.cpp")
if(NOT FM2_PPC_SOURCES)
    message(FATAL_ERROR "out/ppc is empty: run XenonRecomp first (see README)")
endif()
add_library(fm2_ppc STATIC ${FM2_PPC_SOURCES})
target_include_directories(fm2_ppc PUBLIC "${FM2_PPC_DIR}" thirdparty/XenonRecomp/thirdparty/simde)
target_compile_options(fm2_ppc PRIVATE -Wno-everything)
target_compile_options(fm2_ppc PUBLIC -msse4.1 -fno-strict-aliasing)
# Function names in crash reports and profiles.
target_compile_options(fm2_ppc PRIVATE -gline-tables-only "/FI${CMAKE_SOURCE_DIR}/src/ppc_hooks.h")

file(GLOB_RECURSE FM2_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(fm2 ${FM2_SOURCES})
target_include_directories(fm2 PRIVATE src)
target_link_libraries(fm2 PRIVATE fm2_ppc XenonUtils synchronization winmm onecore)
target_compile_definitions(fm2 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(fm2 PRIVATE -gline-tables-only)
target_link_options(fm2 PRIVATE /STACK:0x1000000 /DEBUG)
target_link_libraries(fm2 PRIVATE dbghelp)
