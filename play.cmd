@echo off
rem Starts the FM2 native port from the repository root (game\ must hold the
rem extracted disc). Logs go to out\play.log.
cd /d "%~dp0"
if not exist out mkdir out
build\tu\fm2.exe > out\play.log 2>&1
