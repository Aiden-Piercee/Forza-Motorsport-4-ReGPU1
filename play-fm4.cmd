@echo off
rem Starts FM4 windowed at 720p (1280x720) from the repository root: build\fm4\fm4.exe (cmake --build
rem build\fm4 --target fm4), with game\ holding the extracted disc. Logs go to out\play-fm4.log.
rem Fences are written when recorded and vertex and index data are read in place (the fast path); resolves
rem up to 1024 pixels are written back to guest memory. VSync off, frames capped at 360.
cd /d "%~dp0"
if not exist out mkdir out
if not exist build\fm4\fm4.exe (echo build\fm4\fm4.exe not found: build the fm4 target first & exit /b 1)
set FM2_NO_LAUNCHER=1
set FM2_FULLSCREEN=0
set FM2_RESOLUTION=1280x720
rem Upscaler: off, fsr1, fsr3 or fsr4 (fsr4 = AMD RDNA 4 only, else FSR 3). Quality: 1 quality, 2 balanced,
rem 3 performance, 4 ultra performance. fsr3 and fsr4 need the FidelityFX DLLs beside fm4.exe (the build copies them).
set FM2_UPSCALER=off
set FM2_UPSCALE_QUALITY=2
rem Performance log: frame timing lines in out\play-fm4.log. Delete the line to turn it off.
set REGPU_PERF=1
rem Shader optimiser (builds faster shaders in the background, used on the next start): remove "rem" to turn on; level 1-3.
rem set REGPU_OPTIMIZE_SHADERS=1
rem set REGPU_OPTIMIZE_LEVEL=2
rem Upscaler debug view (needs fsr3 or fsr4): remove "rem" to show the motion vectors or the depth instead of the picture.
rem set REGPU_VIEW_MOTION=1
rem set REGPU_VIEW_DEPTH=1
set FM2_VSYNC=0
set FM2_MAX_FPS=360
set REGPU_EARLY_FENCES=1
set REGPU_RESOLVE_WRITEBACK_MAX=1024
build\fm4\fm4.exe game > out\play-fm4.log 2>&1
