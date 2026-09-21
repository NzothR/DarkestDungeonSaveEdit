#pragma once

#include <cstdint>
#include <string>

namespace ddse::application {

// Stable, transport-independent status returned by the local Gateway.
struct ApplicationStatus {
    std::uint32_t api_version{1};
    std::string application_name;
    std::string application_version;
    std::string state;
};

class ApplicationStatusService {
public:
    [[nodiscard]] ApplicationStatus get_status() const;
};

} // namespace ddse::application
