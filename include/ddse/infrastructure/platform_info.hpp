#pragma once

#include <string_view>

namespace ddse::infrastructure {
[[nodiscard]] std::string_view platform_name() noexcept;
}
