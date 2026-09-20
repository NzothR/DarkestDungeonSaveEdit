#include "ddse/application/application_info.hpp"
#include "ddse/infrastructure/platform_info.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << ddse::application::description() << '\n';
        return 0;
    }
    if (argc != 1) {
        std::cerr << "Usage: ddse_cli [--version]\n";
        return 2;
    }
    std::cout << ddse::application::description()
              << " backend skeleton (" << ddse::infrastructure::platform_name()
              << ")\n";
    return 0;
}
