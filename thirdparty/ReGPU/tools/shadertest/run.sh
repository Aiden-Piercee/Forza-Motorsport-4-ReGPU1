#!/bin/sh
# Builds and runs the translator tests. Needs clang++, Vulkan headers and loader, a Vulkan
# device (Mesa's lavapipe: mesa-vulkan-drivers) and dxc (DXC=/path/to/dxc if not on PATH).
#     run.sh                    the tests
#     run.sh survey <ucode dir> translate a shader dump (tools/extractshaders.py) and compile
#                               every translation with dxc
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src"
build="${BUILD:-$here/../../build/shadertest}"
mkdir -p "$build/out"
if [ "$1" = survey ]; then dump=$(cd "$2" && pwd); fi
cd "$build"
c++ -std=c++20 -O1 -o vkrun "$here/vkrun.cpp" -lvulkan
c++ -std=c++20 -O1 -I"$src" -I"$here" -o tests "$here/tests.cpp" "$src/gpu/hlsl.cpp" "$src/gpu/ucode.cpp" "$here/live_switch_stub.cpp"
if [ "$1" = survey ]; then
    c++ -std=c++20 -O1 -I"$src" -o survey "$here/survey.cpp" "$src/gpu/hlsl.cpp" "$src/gpu/ucode.cpp" "$here/live_switch_stub.cpp"
    rm -rf hlsl && ./survey "$dump" hlsl
    failed=0
    for f in hlsl/*.hlsl; do
        case "$(basename "$f")" in ps_*) stage=ps_6_0 ;; *) stage=vs_6_0 ;; esac
        if ! "${DXC:-dxc}" -HV 2016 -T $stage -E main "$f" -Fo /dev/null > "$f.log" 2>&1; then
            failed=$((failed + 1)); echo "dxc: $f"; head -3 "$f.log"
        fi
    done
    echo "dxc: $failed translation(s) failed to compile"
    exit 0
fi
./tests
