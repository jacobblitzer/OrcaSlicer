# CLAUDE.md

OrcaSlicer — open-source C++17 3D slicer. wxWidgets GUI, CMake build system.

## Build Commands

```bash
# macOS
cmake --build build/arm64 --config RelWithDebInfo --target all --

# Linux
cmake --build build --config RelWithDebInfo --target all --

# Windows (replace %build_type% with Debug/Release/RelWithDebInfo)
cmake --build . --config %build_type% --target ALL_BUILD -- -m
```

### Post-build: Desktop shortcut (Windows)

After every successful Windows build, create/replace a desktop shortcut named `devorcaslicer` pointing at the freshly built `orca-slicer.exe`. Overwrite any existing shortcut so the link always reflects the latest build.

- Target: `C:\Repos\OrcaSlicer\OrcaSlicer\build\src\%build_type%\orca-slicer.exe` (use the same `%build_type%` as the build, e.g. `RelWithDebInfo` or `Release`)
- Working directory: same folder as the target
- Shortcut path: `$env:USERPROFILE\Desktop\devorcaslicer.lnk`

PowerShell one-liner:

```powershell
$bt = 'RelWithDebInfo'  # match build config
$lnk = "$env:USERPROFILE\Desktop\devorcaslicer.lnk"
$tgt = "C:\Repos\OrcaSlicer\OrcaSlicer\build\src\$bt\orca-slicer.exe"
$s = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
$s.TargetPath = $tgt; $s.WorkingDirectory = (Split-Path $tgt); $s.Save()
```

## Testing

Catch2 framework. Tests in `tests/` directory.

```bash
cd build && ctest --output-on-failure           # all tests
ctest --test-dir ./tests/libslic3r              # individual suite
ctest --test-dir ./tests/fff_print
```

## Code Style

- C++17, selective C++20. PascalCase classes, snake_case functions/variables
- `#pragma once` for headers. Smart pointers and RAII preferred
- Parallelization via TBB — be mindful of shared state

## Key Entry Points

- App startup: `src/OrcaSlicer.cpp`
- Slicing pipeline: `src/libslic3r/Print.cpp`
- All print/printer/material settings: `src/libslic3r/PrintConfig.cpp`
- GUI: `src/slic3r/GUI/`
- Core algorithms: `src/libslic3r/` (GCode/, Fill/, Support/, Geometry/, Format/, Arachne/)
- Printer profiles: `resources/profiles/[manufacturer].json`

## Critical Constraints

- **Backward compatibility required** for .3mf project files and printer profiles
- **Cross-platform** — all changes must work on Windows, macOS, and Linux
- Profile/format changes need version migration handling
- Dependencies built separately in `deps/build/`, then linked to main app
