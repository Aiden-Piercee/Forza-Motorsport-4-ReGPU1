#!/bin/sh
# Builds and runs the translator tests. Needs clang++, Vulkan headers and loader, a Vulkan
# device (Mesa's lavapipe: mesa-vulkan-drivers) and dxc (DXC=/path/to/dxc if not on PATH).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src"
build="${BUILD:-$here/../../build/shadertest}"
mkdir -p "$build/out"
cd "$build"
c++ -std=c++20 -O1 -o vkrun "$here/vkrun.cpp" -lvulkan
c++ -std=c++20 -O1 -I"$src" -I"$here" -o tests "$here/tests.cpp" "$src/gpu/hlsl.cpp" "$src/gpu/ucode.cpp" "$here/live_switch_stub.cpp"
./tests
