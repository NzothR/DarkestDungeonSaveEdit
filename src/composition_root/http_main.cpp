#include "ddse/application/application_status.hpp"
#include "ddse/infrastructure/http_drogon_server.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

void print_usage() {
    std::cout << "Usage: ddse_http [--port <0-65535>] [--no-browser]\n"
                 "  --port 0 asks the operating system to choose a free local port.\n";
}

bool parse_port(std::string_view text, std::uint16_t& port) {
    unsigned int parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > 65535)
        return false;
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::uint16_t port = 0;
    bool open_browser = true;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        }
        if (argument == "--no-browser") {
            open_browser = false;
            continue;
        }
        if (argument == "--port" && index + 1 < argc) {
            if (!parse_port(argv[++index], port)) {
                std::cerr << "Invalid local HTTP port.\n";
                print_usage();
                return 2;
            }
            continue;
        }
        std::cerr << "Unknown argument: " << argument << '\n';
        print_usage();
        return 2;
    }

    auto executable_directory = std::filesystem::current_path();
    if (argc > 0 && argv[0] != nullptr) {
        const auto executable_path = std::filesystem::absolute(argv[0]);
        if (executable_path.has_parent_path()) executable_directory = executable_path.parent_path();
    }
    const auto web_root = executable_directory / "web";
    const ddse::application::ApplicationStatusService status_service;
    return ddse::infrastructure::run_drogon_http_server(
        status_service, web_root, port, open_browser);
}
