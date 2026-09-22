#pragma once

#include "ddse/application/configuration_store.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace ddse::application {

// Stable, transport-independent status returned by the local Gateway.
struct ApplicationStatus {
    std::uint32_t api_version{1};
    std::string application_name;
    std::string application_version;
    std::string state;
    std::string configuration_state;
    std::string backup_root;
    bool auto_edit_save_enabled{true};
    std::uint32_t auto_edit_save_interval_seconds{30};
    bool recovery_available{};
};

class ApplicationStatusService {
public:
    explicit ApplicationStatusService(const AppConfigurationStore* configuration_store = nullptr)
        : configuration_store_(configuration_store) {}
    [[nodiscard]] ApplicationStatus get_status() const;

private:
    const AppConfigurationStore* configuration_store_{};
};

} // namespace ddse::application
