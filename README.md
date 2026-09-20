# DDSE backend — Stage 0.7

C++20 / CMake backend with structured errors, logging, a filesystem port and native
adapter, a small SQLite RAII layer, migrations, and an initial DSON reader with structural
diagnostics. SQLite here is infrastructure only; domain repositories are not implemented.

## Requirements

- CMake >= 3.20, a C++20 compiler, and the SQLite3 development package (header + library).
- First CMake configure needs internet access to download GoogleTest v1.15.2, unless
  GTest v1.15.2 is already installed as a CMake package. Subsequent builds reuse it.
- If SQLite3 is installed in a non-standard prefix, set `CMAKE_PREFIX_PATH` to that
  installation prefix in CLion's CMake profile.

## CLion (Windows 10)

Open this directory as the CMake project. Allow CLion to reload CMake after replacing
Stage 0.1 files. If using MinGW or MSVC, keep the same toolchain selected for the whole
project. If a previously configured build directory contains incompatible compiler/cache
settings, use a fresh build directory or delete its CMake cache and reload.

Choose the `ddse_tests` target or run individual test cases in CLion. New cases cover
Result/Error, structured logging, binary filesystem I/O and missing-file errors, SQLite
transaction rollback, and idempotent migrations.

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

Core contains only portable error/result types. Application defines logger and filesystem
ports and links Core. Infrastructure implements those ports and owns SQLite; it depends
inward on Application and Core. The composition root assembles the concrete pieces.
GoogleTest is only linked to `ddse_tests`.

The CLI creates `ddse-data/base_content.db` under its working directory and runs the
bootstrap migration. `--version` exits before initializing infrastructure. The DSON
inspector is read-only:

```powershell
ddse_cli --inspect-dson path/to/persist.game.json
ddse_cli --inspect-dson path/to/persist.roster.json --fields
```

The second command also prints ordered field paths, inferred types, source offsets, and
raw prefixes for unknown field kinds, including fields inside embedded DSON documents.

## Next

Stage 2: DSON Writer and round-trip validation.
