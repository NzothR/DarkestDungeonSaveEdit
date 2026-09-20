#pragma once

#include <filesystem>

namespace ddse::test {
inline std::filesystem::path fixture_root() {
    return std::filesystem::path{DDSE_TEST_FIXTURE_DIR};
}
}  // namespace ddse::test
