@echo off
rem RECORDING: every pad/keyboard input is written to out\sun_input.txt, so the route can be
rem replayed without a person (FM2_REPLAY_INPUT), and the occlusion queries are logged.
rem Go: start a race on the track where the sun shows through the trees and the bridge, drive
rem until the sun sits behind them, wait ~5 seconds, then close the window.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_NO_LAUNCHER=1
set FM2_SKIP_INTRO=1
set FM2_TRACE_ZPD=1
set FM2_RECORD_INPUT=out\sun_input.txt
build\tu\fm2.exe > out\play.log 2>&1
