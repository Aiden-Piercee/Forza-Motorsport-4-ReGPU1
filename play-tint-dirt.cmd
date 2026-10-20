@echo off
rem DEBUG: paints 6 of the car body's pixel shaders in flat colours, to see which one draws
rem the dirt streaks on the bonnet. The colours, in order: red, green, blue, yellow, magenta, cyan.
rem Go: race until the streaks would show, press F12 (or take a screenshot), close the window.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_NO_LAUNCHER=1
set FM2_SKIP_INTRO=1
rem (shared canonical shader copies would skip the tint)
set FM2_NO_CANONICAL=1
set FM2_TINT_PS=25230b3885b57718,2d27d438f58798a0,82658430577ee60f,a6e6592dc4500077,b7b2fdb1adeb69a1,c4e283fd6fc8472e
build\tu\fm2.exe > out\play.log 2>&1
