@echo off
echo === Starting OrcaSlicer Build ===
echo.

REM Set up VS2022 environment
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64

REM Put Strawberry Perl first in PATH AFTER VsDevCmd (before MSYS perl)
set PATH=C:\Strawberry\perl\bin;C:\Program Files\NASM;%PATH%
if errorlevel 1 (
    echo ERROR: Failed to set up VS2022 environment
    exit /b 1
)
echo VS2022 environment loaded.

REM Verify tools
where cmake
if errorlevel 1 (
    echo ERROR: cmake not found in PATH
    exit /b 1
)
cmake --version

REM Set working directory
set WP=%~dp0
echo Working directory: %WP%

REM Build deps
set build_type=Release
set build_dir=build

echo.
echo === Building Dependencies ===
cd /d "%WP%deps"
if not exist %build_dir% mkdir %build_dir%
cd %build_dir%

echo Running cmake configure for deps...
set CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake ../ -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=%build_type%
if errorlevel 1 (
    echo ERROR: cmake configure for deps failed
    exit /b 1
)

echo Running cmake build for deps...
cmake --build . --config %build_type% --target deps -- -m
if errorlevel 1 (
    echo ERROR: cmake build for deps failed
    exit /b 1
)

echo.
echo === Building OrcaSlicer ===
cd /d "%WP%"
if not exist %build_dir% mkdir %build_dir%
cd %build_dir%

echo Running cmake configure for slicer...
set CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake .. -G "Visual Studio 17 2022" -A x64 -DORCA_TOOLS=ON -DCMAKE_BUILD_TYPE=%build_type%
if errorlevel 1 (
    echo ERROR: cmake configure for slicer failed
    exit /b 1
)

echo Running cmake build for slicer...
cmake --build . --config %build_type% --target ALL_BUILD -- -m
if errorlevel 1 (
    echo ERROR: cmake build for slicer failed
    exit /b 1
)

echo Running install...
cmake --build . --target install --config %build_type%

echo.
echo === Build Complete ===
