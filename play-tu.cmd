@echo off
rem Starts the FM2 native port built for the title update (build\tu\fm2.exe), which gives
rem DLC support. Needs default.xexp in Documents\fm2\update and the packages in
rem Documents\fm2\dlc. Logs go to out\play.log.
cd /d "%~dp0"
if not exist out mkdir out
build\tu\fm2.exe > out\play.log 2>&1
