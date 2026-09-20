# Stage 0.1 build decisions

- C++20 with CMake >= 3.20.
- Each library exposes only its own include surface and outward dependencies.
- Core has no I/O library dependency; application links core; infrastructure links core.
- The composition root is the only executable that assembles both application and infrastructure.
- CTest smoke checks exercise linking; a dedicated test framework is deferred to Stage 0.2.
- The version constants are temporary scaffold metadata, not a finalized versioning system.
