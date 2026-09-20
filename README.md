# DDSE backend — Stage 0.2

C++20 / CMake project skeleton with GoogleTest tests. No DSON/SQLite business code yet.

## Requirements

- CMake >= 3.20 and a C++20 compiler (Windows 10 + CLion supported).
- First CMake configure needs internet access to download GoogleTest v1.15.2, unless
  GTest v1.15.2 is already installed as a CMake package. Subsequent builds reuse it.

## CLion (Windows 10)

Open this directory as the CMake project. Allow CLion to reload CMake after replacing
Stage 0.1 files. If using MinGW or MSVC, keep the same toolchain selected for the whole
project. If a previously configured build directory contains incompatible compiler/cache
settings, use a fresh build directory or delete its CMake cache and reload.

Choose the `ddse_tests` target or run individual `CoreVersion.*`, `ApplicationInfo.*`,
`PlatformInfo.*`, or `FixtureLayout.*` GoogleTest cases in CLion.

## Build and test (PowerShell)

```powershell
cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug --config Debug
ctest --test-dir cmake-build-debug -C Debug --output-on-failure
```

For a multi-config generator, build and test with `--config Debug` / `-C Debug`.
For a single-config CLion generator, CMAKE_BUILD_TYPE controls the build configuration.
CLI smoke checking remains registered with CTest. Fixture data is independent of the
current working directory.

## Test structure

- `tests/unit/`: isolated component contract tests.
- `tests/integration/`: cross-component / fixture integration tests.
- `tests/fixtures/{dson,saves,content,environments}/`: deterministic input data.
- `tests/helpers/`: reusable testing utilities (test-only).

`ddse_tests` retains its executable name for existing CLion configurations, but now
runs GoogleTest's main. CTest discovers each individual GoogleTest test at test time.

## Dependency direction

The dependency graph from Stage 0.1 is unchanged. GoogleTest is only used by
`ddse_tests`; core, application, infrastructure, and CLI have no testing dependency.

## Next

Stage 0.3: unified Result/Error types and error contract tests.
