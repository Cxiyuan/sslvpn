@echo off
REM SD-WAN Service Start Script
cd /d "%~dp0"
start "" zerotier-one_x64.exe -C "."
exit /b 0
