#include "ddse/application/application_status.hpp"

#include "ddse/core/version.hpp"

namespace ddse::application {

ApplicationStatus ApplicationStatusService::get_status() const {
    return {
        .api_version = 1,
        .application_name = "Darkest Dungeon Save Editor",
        .application_version = std::string{core::version()},
        .state = "ready",
    };
}

} // namespace ddse::application
