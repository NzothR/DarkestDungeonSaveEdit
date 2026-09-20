# DDSE backend — Stage 5

C++20 / CMake backend with structured errors, logging, a filesystem port and native
adapter, a small SQLite RAII layer, DSON read/write support, Raw Save Profile
discovery, and read-only Vanilla/DLC and Mod content scanners that build atomic
`base_content.db` and `mod_environment.db` catalogs. The mod scanner is configured
with Workshop roots, extra local mod roots (the game manager's `modes` directory
in the current setup), an optional save profile, and an optional manager JSON
export. It inventories installed mods while applying only enabled entries to the
effective content view. SQLite here is infrastructure only; domain repositories
are not implemented.

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

Inspect a profile without modifying it:

```powershell
ddse_cli --inspect-profile path/to/profile_0
ddse_cli --discover-profiles path/to/save-root
```

This reports detected save domains, the profile baseline fingerprint, registered files,
which documents decoded as DSON, and any document-level diagnostics. Unknown filenames
and non-DSON files are retained in the raw registry.

Build the base content database from the default Darkest Dungeon installation roots:

```powershell
ddse_cli --scan-base-content "D:\SteamLibrary\steamapps\common\DarkestDungeon" ".\cmake-build-debug\base_content.db"
```

The default scanner configuration selects the vanilla content directories and direct
DLC directories, and excludes any nested `mods` or `modes` directories. The database
output must be outside every scanned game/DLC root. Text definitions and XML string
tables are parsed; assets are indexed by path, extension, and size without reading their
contents. Rebuilds use a temporary database and replace the previous database only after
a successful transaction. The scan summary includes source and content counts plus
diagnostics for unsupported/binary payloads.

Build a mod environment catalog from Workshop mods, an extra local-mod directory,
the save's active mod list, and a standard JSON export from the official mod manager:

```powershell
ddse_cli --scan-mod-environment `
  ".\test_save_profile\profile_0" `
  "D:\SteamLibrary\steamapps\workshop\content\262060" `
  "D:\SteamLibrary\steamapps\common\DarkestDungeon\modes" `
  "D:\游戏mod\暗黑地牢\mod排序\Default.json" `
  ".\cmake-build-debug\base_content.db" `
  ".\cmake-build-debug\mod_environment.db"
```

Use `-` for an omitted save profile, local-mod root, or manager export. The CLI
currently accepts one local root; application configuration supports multiple
extra local roots. All immediate mod directories are inventoried, including
disabled mods. The manager export's enabled entries and array order are preferred
for the effective view; if it is absent, the scanner uses the save order. When
both sources are available, the database retains both lists and their comparison.
An enabled mod missing from configured roots is diagnosed and skipped from the
effective view rather than guessed. Text content is parsed by the shared scanner;
artwork, audio, and other recognized assets are indexed by metadata. The output
must be outside configured mod roots, the save profile, and the base database.
Rebuild is transactional and atomically replaces the previous output only after
a successful build.

## Next

Stage 6: Composition Root and HTTP API.
