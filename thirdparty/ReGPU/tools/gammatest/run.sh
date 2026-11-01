#!/bin/sh
# Builds and runs the display gamma ramp tests (src/gpu/gamma_ramp.cpp).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src"
build="${BUILD:-$here/../../build/gammatest}"
mkdir -p "$build"
c++ -std=c++20 -O1 -Wall -Wextra -I"$src" -o "$build/tests" "$here/tests.cpp" "$src/gpu/gamma_ramp.cpp"
"$build/tests"
