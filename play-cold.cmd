@echo off
rem Starts the FM2 native port as a first boot: moves the shader and pipeline caches
rem aside (to Documents\fm2\cold-backup-<time>) so every shader compiles again, then runs
rem with FM2_PERF=1. Slow frames and stalls are logged to out\play.log.
cd /d "%~dp0"
if not exist out mkdir out
set "U=%USERPROFILE%\Documents\fm2"
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%t"
set "B=%U%\cold-backup-%STAMP%"
mkdir "%B%" 2>nul
for %%f in (shadercache pipelines.bin pipelines.manifest static_shaders.bin) do (
    if exist "%U%\%%f" move "%U%\%%f" "%B%\" >nul
)
echo Caches moved to %B%
set FM2_PERF=1
build\rel\fm2.exe > out\play.log 2>&1
