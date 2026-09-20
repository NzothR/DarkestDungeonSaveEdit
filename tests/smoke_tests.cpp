#include "ddse/application/application_info.hpp"
#include "ddse/core/version.hpp"
#include "ddse/infrastructure/platform_info.hpp"

#include <iostream>
#include <string_view>

int main() {
    if (ddse::core::version() != "0.1.0") {
        std::cerr << "Unexpected core version\n";
        return 1;
    }
    if (ddse::application::description() != "DDSE 0.1.0") {
        std::cerr << "Application linkage failed\n";
        return 1;
    }
    if (ddse::infrastructure::platform_name().empty()) {
        std::cerr << "Infrastructure linkage failed\n";
        return 1;
    }
    std::cout << "Stage 0.1 smoke tests passed\n";
    return 0;
}
