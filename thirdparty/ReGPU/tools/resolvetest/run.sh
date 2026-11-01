#!/bin/sh
# Builds and runs the resolve destination packing tests (src/gpu/resolve_pack.cpp).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src"
build="${BUILD:-$here/../../build/resolvetest}"
mkdir -p "$build"
c++ -std=c++20 -O1 -Wall -Wextra -I"$src" -o "$build/tests" "$here/tests.cpp" "$src/gpu/resolve_pack.cpp"
"$build/tests"
