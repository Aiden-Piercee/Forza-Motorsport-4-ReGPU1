@echo off
rem Copies the title update (update\) and the downloadable content (dlc\) that ship next to
rem this script into Documents\fm2, where the game looks for them. Files already there are
rem left alone, so this is quick after the first run. (play.cmd, play-cold.cmd and
rem benchmark.cmd call it.)
set "U=%USERPROFILE%\Documents\fm2"
if exist "%~dp0update\default.xexp" robocopy "%~dp0update" "%U%\update" /E /XC /XN /XO /NFL /NDL /NJH /NJS /NP >nul
if exist "%~dp0dlc" robocopy "%~dp0dlc" "%U%\dlc" /E /XC /XN /XO /NFL /NDL /NJH /NJS /NP >nul
exit /b 0
