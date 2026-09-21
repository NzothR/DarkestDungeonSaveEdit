#pragma once

#include "ddse/application/application_status.hpp"

#include <cstdint>
#include <filesystem>

namespace ddse::infrastructure {

// Runs the local-only HTTP gateway until the process receives a shutdown signal.
// A requested port of zero asks the operating system to assign an available port.
[[nodiscard]] int run_drogon_http_server(
    const application::ApplicationStatusService& status_service,
    const std::filesystem::path& web_root,
    std::uint16_t requested_port = 0,
    bool open_browser = true);

} // namespace ddse::infrastructure
