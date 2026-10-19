@echo off
rem RECORDING: runs the title-updated build with every pad/keyboard input written to
rem out\livery_input.txt, so the route can be replayed without a person (FM2_REPLAY_INPUT).
rem Go to the showroom, pick an R-class car with a livery, wait ~5 seconds with it on screen, then quit.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_RECORD_INPUT=out\livery_input.txt
build\tu\fm2.exe > out\play.log 2>&1
