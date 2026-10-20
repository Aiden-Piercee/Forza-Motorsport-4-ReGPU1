@echo off
rem DEBUG: the car's live reflection (the cube map the game renders each frame) is replaced
rem by a plain fallback. If the bonnet streaks disappear, they come from the reflection.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_NO_LAUNCHER=1
set FM2_SKIP_INTRO=1
set FM2_NO_LIVE_CUBE=1
build\tu\fm2.exe > out\play.log 2>&1
