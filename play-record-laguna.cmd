@echo off
rem RECORDING: like play.cmd, but every pad/keyboard input is written to out\laguna_input.txt,
rem so the route can be replayed without a person (FM2_REPLAY_INPUT). Drive to the black
rem patch at Laguna Seca, stop with it in view, wait ~5 seconds, then quit.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_RECORD_INPUT=out\laguna_input.txt
build\rel\fm2.exe > out\play.log 2>&1
