@echo off
rem RECORDING for audio bugs: runs the title-updated build, writes every input to
rem out\audio_input.txt (replay with FM2_REPLAY_INPUT) and everything the game plays to
rem out\audio_play.wav (about 70 MB a minute), plus the music stream log in out\play.log.
rem In a race, hold full throttle until the engine sits at max rpm for a few seconds, then leave
rem the race, wait on the main menu until the next track plays, then quit.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_RECORD_INPUT=out\audio_input.txt
set FM2_DUMP_AUDIO=out\audio_play.wav
set FM2_TRACE_XMA=1
set FM2_TRACE_READS=Music
build\tu\fm2.exe > out\play.log 2>&1
