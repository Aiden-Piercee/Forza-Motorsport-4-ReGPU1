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
set FM2_UPSCALER=off
set FM2_VSYNC=0
set FM2_MAX_FPS=360
set REGPU_EARLY_FENCES=1
set REGPU_RESOLVE_WRITEBACK_MAX=1024
build\fm4\fm4.exe game > out\play-fm4.log 2>&1
