@echo off
rem RECORDING: a brand-new profile, so the game creates its career and the starter car's garage
rem thumbnail. Inputs go to out\newcareer_input.txt (replay with FM2_REPLAY_INPUT and a fresh
rem FM2_PROFILE). The profile "thumbtest" is deleted first so every recording starts clean.
rem Create the career, pick the starting car, open the garage / car screen where its thumbnail
rem shows, wait ~5 seconds, then quit.
cd /d "%~dp0"
if not exist out mkdir out
if exist "%USERPROFILE%\Documents\fm2\profiles\thumbtest" rmdir /s /q "%USERPROFILE%\Documents\fm2\profiles\thumbtest"
set FM2_PROFILE=thumbtest
set FM2_NO_LAUNCHER=1
set FM2_RECORD_INPUT=out\newcareer_input.txt
build\tu\fm2.exe > out\play.log 2>&1
