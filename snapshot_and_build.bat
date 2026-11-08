@echo off
REM ===========================================================================
REM  Snapshot the OrcaSlicer datadir, then build.
REM
REM  Every argument is passed straight through to build_win.bat, so this is a
REM  drop-in replacement for it:
REM
REM      snapshot_and_build.bat -d -s -i          full build
REM      snapshot_and_build.bat -s -i --no-configure    rebuild after edits
REM
REM  Why it exists: a routine rebuild never touches your config, but syncing to
REM  a new upstream version changes SoftFever_VERSION in version.inc, and on a
REM  version change OrcaSlicer migrates %APPDATA%\OrcaSlicer and re-runs the
REM  printer setup flow.  Its own user_backup-v<version> snapshot covers only
REM  user\, so this takes a fuller copy first.
REM
REM  Set ORCA_DATADIR to snapshot a datadir other than the default.
REM ===========================================================================

setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "DATADIR=%APPDATA%\OrcaSlicer"
if not "%ORCA_DATADIR%"=="" set "DATADIR=%ORCA_DATADIR%"
set "SNAPROOT=%LOCALAPPDATA%\OrcaSlicer-config-snapshots"
set "KEEP=20"

REM --- OrcaSlicer writes its config on exit, so a snapshot taken while it is
REM --- running would miss whatever is still only in memory.
tasklist /FI "IMAGENAME eq orca-slicer.exe" 2>nul | find /I "orca-slicer.exe" >nul
if not errorlevel 1 (
    echo ERROR: OrcaSlicer is running.
    echo        It writes its config on exit, so a snapshot now would be incomplete.
    echo        Close it and run this again.
    exit /b 1
)

if not exist "%DATADIR%" (
    echo No datadir at "%DATADIR%" - nothing to snapshot.
    goto :build
)

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HHmmss"') do set "STAMP=%%i"
set "SNAP=%SNAPROOT%\%STAMP%"
mkdir "%SNAP%" 2>nul

echo Snapshotting "%DATADIR%"
echo          to "%SNAP%"

REM Skips the four directories OrcaSlicer rebuilds by itself, which are ~105 MB
REM of the ~106 MB datadir: plugins\ (re-downloaded on demand), system\ (unpacked
REM from resources\profiles), python\ (installed by the build), plus logs, caches
REM and the OTA staging dir.  What is left - OrcaSlicer.conf, user\, printers\,
REM orca_plugins\ and the user_backup-* folders - is the part you cannot recreate.
robocopy "%DATADIR%" "%SNAP%" /E /NFL /NDL /NJH /NJS /R:1 /W:1 /XD plugins system python log cache ota >nul
if errorlevel 8 (
    echo ERROR: snapshot failed, robocopy exit %ERRORLEVEL%. Not building.
    exit /b 1
)

REM --- record which build this config belonged to ---
> "%SNAP%\SNAPSHOT.txt" (
    echo Snapshot of %DATADIR%
    echo Taken   : %STAMP%
    echo.
    findstr /C:"set(SoftFever_VERSION" version.inc
    echo.
    echo Branch  :
    git rev-parse --abbrev-ref HEAD 2>nul
    echo Commit  :
    git rev-parse --short HEAD 2>nul
    echo.
    echo To restore, with OrcaSlicer closed:
    echo   robocopy "%SNAP%" "%DATADIR%" /E
)

echo Snapshot done.

REM --- keep only the newest %KEEP%; names sort chronologically ---
for /f "skip=%KEEP% delims=" %%d in ('dir /b /ad /o-n "%SNAPROOT%" 2^>nul') do (
    echo Pruning old snapshot %%d
    rmdir /s /q "%SNAPROOT%\%%d"
)

:build
echo.
echo === build_win.bat %* ===
call "%~dp0build_win.bat" %*
exit /b %ERRORLEVEL%
