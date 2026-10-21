@echo off
rem RECORDING: runs the title-updated build with every pad/keyboard input written to
rem out\photo_input.txt, so the route can be replayed without a person (FM2_REPLAY_INPUT).
rem Start a race, pause, open Photo Mode and move the frame around until the picture goes black. Keep moving for ~5 seconds, then quit.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_RECORD_INPUT=out\photo_input.txt
build\tu\fm2.exe > out\play.log 2>&1
