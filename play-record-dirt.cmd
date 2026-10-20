@echo off
rem RECORDING: every pad/keyboard input is written to out\dirt_input.txt, so the race can be
rem replayed without a person (FM2_REPLAY_INPUT); F12 also logs the vertex constants and the
rem car body's per-vertex dirt values.
rem Go: start a race (the AE86 at Tsukuba shows it), drive until the streaks show on the bonnet,
rem press F12, wait a few seconds, then close the window.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_NO_LAUNCHER=1
set FM2_SKIP_INTRO=1
set FM2_TRACE_VC=1
set FM2_TRACE_VSTAT=3,15
set FM2_RECORD_INPUT=out\dirt_input.txt
build\tu\fm2.exe > out\play.log 2>&1
