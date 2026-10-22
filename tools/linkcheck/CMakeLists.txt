# Link check for milestone 1: compiles the three recompiled FM4 images (out/ppc,
# out/ppc_xmf, out/ppc_sph) and links them with generated import stubs and the
# image table, without the runtime. See docs/FM4-RECOMPILE.md.
cmake_minimum_required(VERSION 3.25)
project(fm4_linkcheck C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "the recompiled code needs clang-cl (it uses clang builtins)")
endif()
get_filename_component(FM4_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
# clang-cl on Windows; plain clang also works, for checking the tools elsewhere.
if(MSVC)
    set(FM4_FORCE_INCLUDE "/FI${FM4_ROOT}/src/ppc_hooks.h")
else()
    set(FM4_FORCE_INCLUDE -include "${FM4_ROOT}/src/ppc_hooks.h")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

# One static library per image: each keeps its own ppc_config.h.
set(FM4_IMAGES "fm4:ppc" "fm4_xmediafacade:ppc_xmf" "fm4_speechfacade:ppc_sph")
set(FM4_CONFIGS "")
set(FM4_SHARED_HEADERS "")
foreach(image ${FM4_IMAGES})
    string(REPLACE ":" ";" parts "${image}")
    list(GET parts 0 name)
    list(GET parts 1 dir)
    set(ppc_dir "${FM4_ROOT}/out/${dir}")
    file(GLOB sources CONFIGURE_DEPENDS "${ppc_dir}/*.cpp")
    if(NOT sources)
        message(FATAL_ERROR "out/${dir} is empty: run tools/recomp-fm4.sh first")
    endif()
    add_library(${name}_ppc STATIC ${sources})
    target_include_directories(${name}_ppc PRIVATE "${ppc_dir}" "${FM4_ROOT}/thirdparty/XenonRecomp/thirdparty/simde")
    target_compile_options(${name}_ppc PRIVATE -Wno-everything -msse4.1 -fno-strict-aliasing ${FM4_FORCE_INCLUDE})
    list(APPEND FM4_CONFIGS "${FM4_ROOT}/config/${name}.toml")
    list(APPEND FM4_SHARED_HEADERS "${ppc_dir}/ppc_recomp_shared.h")
endforeach()

set(generated "${CMAKE_BINARY_DIR}/generated")
add_custom_command(OUTPUT "${generated}/stubs.cpp"
    COMMAND Python3::Interpreter -I "${FM4_ROOT}/tools/genstubs.py" ${FM4_SHARED_HEADERS} "${generated}/stubs.cpp"
    DEPENDS ${FM4_SHARED_HEADERS} "${FM4_ROOT}/tools/genstubs.py"
    COMMENT "Generating import stubs")
add_custom_command(OUTPUT "${generated}/recomp_modules.cpp"
    COMMAND Python3::Interpreter -I "${FM4_ROOT}/tools/genmodules.py" "${generated}/recomp_modules.cpp" ${FM4_CONFIGS}
    DEPENDS ${FM4_CONFIGS} ${FM4_SHARED_HEADERS} "${FM4_ROOT}/tools/genmodules.py"
    COMMENT "Generating the recompiled image table")

add_executable(fm4_linkcheck main.cpp "${generated}/stubs.cpp" "${generated}/recomp_modules.cpp")
# The shim's kernel/imports.h replaces the runtime's for the stubs.
target_include_directories(fm4_linkcheck PRIVATE "${CMAKE_SOURCE_DIR}/shim" "${FM4_ROOT}/src" "${FM4_ROOT}/out/ppc"
    "${FM4_ROOT}/thirdparty/XenonRecomp/thirdparty/simde")
target_compile_options(fm4_linkcheck PRIVATE -msse4.1 ${FM4_FORCE_INCLUDE})
target_link_libraries(fm4_linkcheck PRIVATE fm4_ppc fm4_xmediafacade_ppc fm4_speechfacade_ppc)
if(MSVC)
    target_link_options(fm4_linkcheck PRIVATE /STACK:0x1000000)
endif()
