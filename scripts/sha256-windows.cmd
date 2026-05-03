@echo off
REM scripts\sha256-windows.cmd <archive-path> <archive-name> <output-sums-file>
REM Writes "<hex>  <name>" to the output sums file (overwrites it).

setlocal enabledelayedexpansion
set "ARCHIVE=%~1"
set "NAME=%~2"
set "OUT=%~3"

if not exist "%ARCHIVE%" (
    echo sha256-windows: archive not found: %ARCHIVE% 1>&2
    exit /b 1
)

set "HASH="
for /f "skip=1 tokens=1" %%H in ('certutil -hashfile "%ARCHIVE%" SHA256') do (
    if not defined HASH (
        REM Skip the trailing "CertUtil: ..." line by checking for hex-only token.
        echo %%H| findstr /R /X "[0-9a-fA-F][0-9a-fA-F]*" >nul
        if not errorlevel 1 set "HASH=%%H"
    )
)

if not defined HASH (
    echo sha256-windows: failed to extract hash 1>&2
    exit /b 1
)

> "%OUT%" echo %HASH%  %NAME%
endlocal
