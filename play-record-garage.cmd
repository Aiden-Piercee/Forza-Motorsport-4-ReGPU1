@echo off
rem RECORDING: runs the title-updated build with every pad/keyboard input written to
rem out\garage_input.txt, so the route can be replayed without a person (FM2_REPLAY_INPUT).
rem Go to the screen with the corrupted garage thumbnail, leave it showing for ~5 seconds, then quit.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_RECORD_INPUT=out\garage_input.txt
build\tu\fm2.exe > out\play.log 2>&1
