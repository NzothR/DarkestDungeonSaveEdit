# DDSE backend — Stage 0.1

Minimal C++20 / CMake skeleton for the Darkest Dungeon 1 Sandbox Save Editor.

## Requirements

- CMake 3.20 or newer
- C++20 compiler (MSVC, GCC or Clang)

## Configure, build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For single-configuration generators on Windows, select the configuration at build and test time:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Run `build/ddse_cli` on Unix-like single-config generators, or `build/Debug/ddse_cli.exe` on common Windows multi-config generators. The exact path depends on the generator.

## Dependency direction

```text
 ddse_cli (composition root)
    |-- ddse_application ---> ddse_core
    `-- ddse_infrastructure -> ddse_core

ddse_tests -> application + infrastructure + core (transitive)
```

Application does not link infrastructure; neither core nor application links SQLite or Drogon.
The CLI and smoke tests are placeholders for validating the build graph, not production services.
A dedicated third-party unit test framework is deferred to Stage 0.2.

## Next

Stage 0.2: introduce a testing framework, fixture organization and focused unit tests.
