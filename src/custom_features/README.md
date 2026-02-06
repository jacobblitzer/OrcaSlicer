# Custom Features

This directory contains custom features designed to be portable across PrusaSlicer-based forks (OrcaSlicer, BambuStudio, PrusaSlicer).

## Structure
Each feature lives in its own subdirectory with self-contained code where possible.

## Integration Points
All modifications to the main codebase are marked with:
// CUSTOM_FEATURE: <feature-name>

This makes it easy to find, extract, and cherry-pick features to other forks.

## Porting Features
1. Copy the feature's subdirectory
2. Search for // CUSTOM_FEATURE: <name> in the codebase
3. Apply those integration points to the target fork
