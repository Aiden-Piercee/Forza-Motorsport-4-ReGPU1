cmake_minimum_required(VERSION 3.25)
project(fm4 C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "fm4 needs clang-cl (the recompiled code uses clang builtins)")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

add_subdirectory(thirdparty/XenonRecomp EXCLUDE_FROM_ALL)

# Recompiled game code, produced by XenonRecomp from your own XEX files (tools/recomp-fm4.sh;
# never committed): one static library per image so each keeps its own ppc_config.h. The
# first image is default.xex, the others are the modules it loads at run time.
set(FM4_IMAGES "fm4:ppc" "fm4_xmediafacade:ppc_xmf" "fm4_speechfacade:ppc_sph")
set(FM4_CONFIGS "")
set(FM4_SHARED_HEADERS "")
set(FM4_PPC_LIBS "")
set(FM4_PPC_DIRS "")
foreach(image ${FM4_IMAGES})
    string(REPLACE ":" ";" parts "${image}")
    list(GET parts 0 name)
    list(GET parts 1 dir)
    set(ppc_dir "${CMAKE_SOURCE_DIR}/out/${dir}")
    file(GLOB sources CONFIGURE_DEPENDS "${ppc_dir}/*.cpp")
    if(NOT sources)
        message(FATAL_ERROR "out/${dir} is empty: run tools/recomp-fm4.sh first (see docs/FM4-RECOMPILE.md)")
    endif()
    add_library(${name}_ppc STATIC ${sources})
    target_include_directories(${name}_ppc PRIVATE "${ppc_dir}" thirdparty/XenonRecomp/thirdparty/simde)
    target_compile_options(${name}_ppc PRIVATE -Wno-everything -msse4.1 -fno-strict-aliasing
        -gline-tables-only "/FI${CMAKE_SOURCE_DIR}/src/ppc_hooks.h")
    # The recompiled code reads the host's raw timestamp counter for mftb; give it the console's
    # timebase instead (tools/patch_mftb.py, Fm2Timebase in src/kernel/runtime.cpp).
    execute_process(COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/patch_mftb.py" "${ppc_dir}"
                    OUTPUT_VARIABLE mftb_result RESULT_VARIABLE mftb_code OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT mftb_code EQUAL 0)
        message(FATAL_ERROR "tools/patch_mftb.py failed on ${ppc_dir}")
    endif()
    message(STATUS "${name} mftb: ${mftb_result}")
    list(APPEND FM4_CONFIGS "${CMAKE_SOURCE_DIR}/config/${name}.toml")
    list(APPEND FM4_SHARED_HEADERS "${ppc_dir}/ppc_recomp_shared.h")
    list(APPEND FM4_PPC_LIBS ${name}_ppc)
endforeach()

# Import stubs for all three images, and the table of recompiled images (src/recomp_modules.h).
set(FM4_GENERATED "${CMAKE_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${FM4_GENERATED}/stubs.cpp"
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/genstubs.py" ${FM4_SHARED_HEADERS} "${FM4_GENERATED}/stubs.cpp"
    DEPENDS ${FM4_SHARED_HEADERS} "${CMAKE_SOURCE_DIR}/tools/genstubs.py"
    COMMENT "Generating import stubs")
add_custom_command(OUTPUT "${FM4_GENERATED}/recomp_modules.cpp"
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/genmodules.py" "${FM4_GENERATED}/recomp_modules.cpp" ${FM4_CONFIGS}
    DEPENDS ${FM4_CONFIGS} ${FM4_SHARED_HEADERS} "${CMAKE_SOURCE_DIR}/tools/genmodules.py"
    COMMENT "Generating the recompiled image table")

# Runtime sources. src/hooks/fm2 holds FM2's address-bound hooks (they override FM2 sub_
# functions, which would hijack unrelated FM4 code); they stay out of the build unless asked.
option(FM4_BUILD_FM2_HOOKS "Compile src/hooks/fm2 (reference only; breaks an FM4 build)" OFF)
file(GLOB_RECURSE FM4_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER FM4_SOURCES EXCLUDE REGEX "src/kernel/stubs\\.cpp$")
if(NOT FM4_BUILD_FM2_HOOKS)
    list(FILTER FM4_SOURCES EXCLUDE REGEX "src/hooks/fm2/")
endif()
list(APPEND FM4_SOURCES "${FM4_GENERATED}/stubs.cpp" "${FM4_GENERATED}/recomp_modules.cpp")

# Upscalers: AMD FidelityFX SDK (MIT) and NVIDIA DLSS SDK, fetched into thirdparty/ (see README).
set(FM4_FFX "${CMAKE_SOURCE_DIR}/thirdparty/fidelityfx/Kits/FidelityFX")
if(NOT EXISTS "${FM4_FFX}/upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h")
    message(FATAL_ERROR "thirdparty/fidelityfx is missing: clone FidelityFX-SDK v2.3.0 there (see README)")
endif()
# FSR 1 compute shaders, compiled with fxc at build time and embedded.
set(FM4_GENERATED "${CMAKE_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${FM4_GENERATED}/fsr1_cs.h"
    COMMAND py -3 -I "${CMAKE_SOURCE_DIR}/tools/embed_hlsl.py" "${CMAKE_SOURCE_DIR}/src/gpu/fsr1.hlsl" "${FM4_GENERATED}/fsr1_cs.h"
            EasuMain,RcasMain,DepthCopyMain,GammaMain,EnhanceMain "${FM4_FFX}/api/internal/gpu" "${FM4_FFX}/upscalers/fsr3/include/gpu/fsr1"
    DEPENDS "${CMAKE_SOURCE_DIR}/src/gpu/fsr1.hlsl" "${CMAKE_SOURCE_DIR}/tools/embed_hlsl.py"
    COMMENT "Compiling FSR 1 shaders")
list(APPEND FM4_SOURCES "${FM4_GENERATED}/fsr1_cs.h")
set(FM4_DLSS "${CMAKE_SOURCE_DIR}/thirdparty/dlss")
if(NOT EXISTS "${FM4_DLSS}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "thirdparty/dlss is missing: clone NVIDIA/DLSS v310.9.16 there (see README)")
endif()
add_executable(fm4 ${FM4_SOURCES} "${CMAKE_SOURCE_DIR}/src/fm2.rc")
target_include_directories(fm4 PRIVATE src "${FM4_GENERATED}" "${FM4_DLSS}/include"
    "${CMAKE_SOURCE_DIR}/out/ppc" thirdparty/XenonRecomp/thirdparty/simde)
target_link_libraries(fm4 PRIVATE "${FM4_DLSS}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib" "${FM4_FFX}/signedbin/amd_fidelityfx_loader_dx12.lib")
target_include_directories(fm4 PRIVATE "${FM4_FFX}/api/include" "${FM4_FFX}/upscalers/include")
add_custom_command(TARGET fm4 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FM4_FFX}/signedbin/amd_fidelityfx_loader_dx12.dll" "${FM4_FFX}/signedbin/amd_fidelityfx_upscaler_dx12.dll" $<TARGET_FILE_DIR:fm4>)
add_custom_command(TARGET fm4 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${FM4_DLSS}/lib/Windows_x86_64/rel/nvngx_dlss.dll" $<TARGET_FILE_DIR:fm4>)
target_link_libraries(fm4 PRIVATE ${FM4_PPC_LIBS} XenonUtils synchronization winmm onecore d3d12 dxgi d3dcompiler)
target_compile_definitions(fm4 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(fm4 PRIVATE -gline-tables-only)
target_link_options(fm4 PRIVATE /STACK:0x1000000 /DEBUG)
target_link_libraries(fm4 PRIVATE dbghelp xaudio2 bcrypt winhttp)

# XMA decoding through FFmpeg's xma2 decoder (LGPL, linked dynamically): a shared build
# with include/, lib/ and bin/ in thirdparty/ffmpeg (see README).
set(FM4_FFMPEG "${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg")
if(NOT EXISTS "${FM4_FFMPEG}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "thirdparty/ffmpeg is missing: extract an FFmpeg win64 LGPL shared build there (see README)")
endif()
target_include_directories(fm4 PRIVATE "${FM4_FFMPEG}/include")
target_link_libraries(fm4 PRIVATE "${FM4_FFMPEG}/lib/avcodec.lib" "${FM4_FFMPEG}/lib/avutil.lib")
file(GLOB FM4_FFMPEG_DLLS "${FM4_FFMPEG}/bin/avcodec-*.dll" "${FM4_FFMPEG}/bin/avutil-*.dll" "${FM4_FFMPEG}/bin/swresample-*.dll")
add_custom_command(TARGET fm4 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FM4_FFMPEG_DLLS} $<TARGET_FILE_DIR:fm4>)

# Xenos shader disassembler for captured microcode.
add_executable(xenos_dis tools/xenos_dis.cpp src/gpu/ucode.cpp src/gpu/hlsl.cpp)
target_include_directories(xenos_dis PRIVATE src)
target_link_libraries(xenos_dis PRIVATE d3dcompiler)
