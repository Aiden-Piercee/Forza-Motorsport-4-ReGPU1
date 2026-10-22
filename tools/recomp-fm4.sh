#!/bin/sh
# Recompile FM4's three XEX images (default.xex, XMediaFacade, SpeechFacade) with
# XenonRecomp into out/ppc, out/ppc_xmf and out/ppc_sph. See docs/FM4-RECOMPILE.md.
#
#   sh tools/recomp-fm4.sh --prepare   dump each image, find the config values and
#                                      write them into config/*.toml, and generate
#                                      the switch tables with XenonAnalyse
#   sh tools/recomp-fm4.sh             recompile (the configs must be prepared)
set -e
cd "$(dirname "$0")/.."

IMAGES="fm4:default.xex:b34dfb88978819cccda6dafd755e7139970a58fbf6707dfbcba06846857f52bd:ppc
fm4_xmediafacade:XMediaFacade_default.xex:2e82a8d08a59e79f4d68e68b744c2872855bfdd583eb39ad0a9c54e5b3fa1492:ppc_xmf
fm4_speechfacade:SpeechFacade_default.xex:49d506a2f8556836943d8bab3a50b6cd3877524500509ef462fb6e9802f2f4bf:ppc_sph"
TOOLS=build/tools/xenonrecomp

for line in $IMAGES; do
    xex=$(echo "$line" | cut -d: -f2)
    sha=$(echo "$line" | cut -d: -f3)
    [ -f "game/$xex" ] || { echo "game/$xex is missing (see docs/FM4-RECOMPILE.md)"; exit 1; }
    [ "$(sha256sum "game/$xex" | cut -d' ' -f1)" = "$sha" ] || { echo "game/$xex: unsupported version (SHA-256 differs)"; exit 1; }
done

cmd //c "tools\vcenv.cmd ninja -C build/tools xenonrecomp/XenonRecomp/XenonRecomp.exe xenonrecomp/XenonAnalyse/XenonAnalyse.exe xexdump.exe" | tail -1
mkdir -p out

if [ "$1" = "--prepare" ]; then
    for line in $IMAGES; do
        name=$(echo "$line" | cut -d: -f1)
        xex=$(echo "$line" | cut -d: -f2)
        build/tools/xexdump.exe "game/$xex" "out/$name.bin" > "out/$name.sections"
        py -3 -I tools/fm4_config.py "out/$name.bin" "out/$name.sections" --write "config/$name.toml" > "out/$name.config.txt"
        cat "out/$name.config.txt"
        "$TOOLS/XenonAnalyse/XenonAnalyse.exe" "game/$xex" "config/${name}_switch_tables.toml"
        echo "$name: $(grep -c '^\[\[switch\]\]' "config/${name}_switch_tables.toml") switch tables"
    done
    echo "Review out/*.config.txt (setjmp/longjmp candidates) and the config diffs, then run without --prepare."
    exit 0
fi

grep -q '^restgprlr_14_address = 0x0$' config/fm4.toml && { echo "config/fm4.toml is not prepared: run with --prepare first"; exit 1; }

for line in $IMAGES; do
    name=$(echo "$line" | cut -d: -f1)
    dir=out/$(echo "$line" | cut -d: -f4)
    rm -rf "$dir" && mkdir -p "$dir"
    "$TOOLS/XenonRecomp/XenonRecomp.exe" "config/$name.toml" thirdparty/XenonRecomp/XenonUtils/ppc_context.h > "out/$name.recomp.log" 2>&1
    grep -v "^Recompiling" "out/$name.recomp.log" || true
    py -3 -I tools/fallthrough.py "$dir" > "out/$name.fallthrough.txt"
    echo "$name: $(grep -c '^\s*{ 0x' "$dir/ppc_func_mapping.cpp") functions, $(wc -l < "out/$name.fallthrough.txt") possible fallthroughs (out/$name.fallthrough.txt)"
done
py -3 -I tools/genstubs.py out/ppc/ppc_recomp_shared.h out/ppc_xmf/ppc_recomp_shared.h out/ppc_sph/ppc_recomp_shared.h src/kernel/stubs.cpp
