#!/bin/sh
# Build the test XEX, recompile it with XenonRecomp, check the disassembly,
# compile the generated code with the harness and run it. Needs clang++.
#   XR      the patched XenonRecomp source (default: thirdparty/XenonRecomp)
#   XR_BIN  the XenonRecomp built from it (default: build/tools/.../XenonRecomp)
# The test XEX is built from hand-encoded instructions; no game code is used.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
XR=${XR:-$HERE/../../thirdparty/XenonRecomp}
XR_BIN=${XR_BIN:-$HERE/../../build/tools/xenonrecomp/XenonRecomp/XenonRecomp}
B=${B:-$HERE/../../out/xenonrecomp-tests}
rm -rf "$B"
python3 "$HERE/gen_tests.py" "$B"
(cd "$B" && "$XR_BIN" config.toml "$XR/XenonUtils/ppc_context.h" > recomp.log 2>&1)
grep -v "address is unspecified\|Recompiling functions" "$B/recomp.log" || true
if grep -q "Unrecognized\|RC bit enabled\|Unable to decode" "$B/recomp.log"; then echo "recompile reported problems"; exit 1; fi
python3 "$HERE/check_disasm.py" "$B"
FLAGS="-std=c++20 -O1 -msse4.1 -fno-strict-aliasing -include $HERE/ppc_hooks_test.h -I$B/out -I$B -I$XR/thirdparty/simde -Wno-unused-label"
for f in "$B"/out/ppc_recomp.*.cpp; do clang++ $FLAGS -c "$f" -o "$f.o" & done
wait
clang++ $FLAGS -c "$HERE/harness.cpp" -o "$B/harness.o"
clang++ "$B"/out/ppc_recomp.*.o "$B/harness.o" -o "$B/harness"
"$B/harness"
