#include "ddse/infrastructure/platform_info.hpp"

namespace ddse::infrastructure {
std::string_view platform_name() noexcept {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#else
    return "Other";
#endif
}
}
