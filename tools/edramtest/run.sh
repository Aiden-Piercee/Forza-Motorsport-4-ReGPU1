#!/bin/sh
# Builds and runs the EDRAM model tests (no GPU needed).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src"
build="${BUILD:-$here/../../build/edramtest}"
mkdir -p "$build"
c++ -std=c++20 -O1 -Wall -Wextra -I"$src" -o "$build/tests" "$here/tests.cpp" "$src/gpu/edram.cpp"
"$build/tests"
