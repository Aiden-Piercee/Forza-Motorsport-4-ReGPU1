#!/bin/sh
# Builds and runs the EDRAM model tests. The transfer shader tests also need a Vulkan device
# (Mesa's lavapipe: mesa-vulkan-drivers), the Vulkan headers and loader, and dxc (DXC=/path/to/dxc
# if not on PATH); without dxc only the model tests run.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src"
build="${BUILD:-$here/../../build/edramtest}"
mkdir -p "$build"
cd "$build"
c++ -std=c++20 -O1 -Wall -Wextra -I"$src" -o tests "$here/tests.cpp" "$src/gpu/edram.cpp"
./tests
if command -v "${DXC:-dxc}" > /dev/null 2>&1; then
    c++ -std=c++20 -O1 -o vkrun "$here/../shadertest/vkrun.cpp" -lvulkan
    c++ -std=c++20 -O1 -Wall -Wextra -I"$src" -o transfer_test "$here/transfer_test.cpp" "$src/gpu/edram.cpp"
    ./transfer_test
else
    echo "dxc not found: transfer shader tests skipped"
fi
