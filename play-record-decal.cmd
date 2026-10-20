@echo off
rem RECORDING: runs the title-updated build on the Fresh1 profile with every pad/keyboard input
rem written to out\decal_input.txt, so the route can be replayed without a person (FM2_REPLAY_INPUT).
rem Go: title (A), Career, Paint Car, the decal/vinyl editor, place a decal on the car, leave it
rem showing for ~5 seconds, then close the window.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_PROFILE=Fresh1
set FM2_NO_LAUNCHER=1
set FM2_SKIP_INTRO=1
set FM2_RECORD_INPUT=out\decal_input.txt
build\tu\fm2.exe > out\play.log 2>&1
