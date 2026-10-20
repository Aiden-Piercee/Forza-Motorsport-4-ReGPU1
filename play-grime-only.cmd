@echo off
rem DEBUG: the car body shows only the grime mask the body shader reads (texture slot 2 at the
rem body's second texture coordinates): white is clean, dark is grime. Paint and lighting are off.
rem Go: race to lap 2, look at the bonnet, press F12, then close the window.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_NO_LAUNCHER=1
set FM2_SKIP_INTRO=1
rem (shared canonical shader copies would skip the replacement)
set FM2_NO_CANONICAL=1
set FM2_DEBUG_PS=6f56ffaa51932b68:float4(TFetch(2, float3(input.o[0].zw, 0), false, false, 0.0, float2(0.0, 0.0)).xyz, 1)
build\tu\fm2.exe > out\play.log 2>&1
