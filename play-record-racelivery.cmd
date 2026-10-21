@echo off
rem RECORDING: runs the title-updated build with every pad/keyboard input written to
rem out\racelivery_input.txt, so the route can be replayed without a person (FM2_REPLAY_INPUT).
rem Start a race with a car that has your custom livery. Once the glitchy texture is visible, wait ~5 seconds, then quit.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_RECORD_INPUT=out\racelivery_input.txt
build\tu\fm2.exe > out\play.log 2>&1
