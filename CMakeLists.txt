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
target_link_libraries(fm2 PRIVATE fm2_ppc XenonUtils synchronization winmm onecore d3d12 dxgi d3dcompiler)
target_compile_definitions(fm2 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(fm2 PRIVATE -gline-tables-only)
target_link_options(fm2 PRIVATE /STACK:0x1000000 /DEBUG)
target_link_libraries(fm2 PRIVATE dbghelp xaudio2)

# XMA decoding through FFmpeg's xma2 decoder (LGPL, linked dynamically): a shared build
# with include/, lib/ and bin/ in thirdparty/ffmpeg (see README).
set(FM2_FFMPEG "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg")
if(NOT EXISTS "${FM2_FFMPEG}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "thirdparty/ffmpeg is missing: extract an FFmpeg win64 LGPL shared build there (see README)")
endif()
target_include_directories(fm2 PRIVATE "${FM2_FFMPEG}/include")
target_link_libraries(fm2 PRIVATE "${FM2_FFMPEG}/lib/avcodec.lib" "${FM2_FFMPEG}/lib/avutil.lib")
file(GLOB FM2_FFMPEG_DLLS "${FM2_FFMPEG}/bin/avcodec-*.dll" "${FM2_FFMPEG}/bin/avutil-*.dll" "${FM2_FFMPEG}/bin/swresample-*.dll")
add_custom_command(TARGET fm2 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FM2_FFMPEG_DLLS} $<TARGET_FILE_DIR:fm2>)

# Xenos shader disassembler for captured microcode.
add_executable(xenos_dis tools/xenos_dis.cpp src/gpu/ucode.cpp src/gpu/hlsl.cpp)
target_include_directories(xenos_dis PRIVATE src)
target_link_libraries(xenos_dis PRIVATE d3dcompiler)
