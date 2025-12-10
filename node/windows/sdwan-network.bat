@echo off
REM SD-WAN Network Join/Leave Script
REM Usage: sdwan-network.bat <join|leave> <network_id>

setlocal
cd /d "%~dp0"

set ACTION=%1
set NETWORK_ID=%2

if "%ACTION%"=="" (
    echo Error: Action not specified
    exit /b 1
)

if "%NETWORK_ID%"=="" (
    echo Error: Network ID not specified
    exit /b 1
)

zerotier-one_x64.exe -q -D"." %ACTION% %NETWORK_ID%
exit /b %ERRORLEVEL%
