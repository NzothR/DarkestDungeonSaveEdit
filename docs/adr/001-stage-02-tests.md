# ADR-001: GoogleTest for Stage 0.2

Use GoogleTest 1.15.2 as a test-only dependency, discovered with CMake's GoogleTest module.
Try an existing GTest CMake package first; otherwise FetchContent downloads the pinned tag.
CTest remains the cross-platform test driver. Fixture paths are passed from CMake, not
resolved relative to CLion's working directory. Production libraries do not link GTest.
If the machine is offline, provide an existing GTest installation and set GTest_DIR,
or pre-populate the FetchContent source directory.
