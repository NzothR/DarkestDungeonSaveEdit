#include "ddse/application/application_status.hpp"

#include "ddse/core/version.hpp"

namespace ddse::application {

ApplicationStatus ApplicationStatusService::get_status() const {
    bool recovery_available = false;
    if (configuration_store_) {
        const auto recovery = configuration_store_->recovery_status();
        recovery_available = recovery && recovery.value().available;
    }
    return {
        .api_version = 1,
        .application_name = "Darkest Dungeon Save Editor",
        .application_version = std::string{core::version()},
        .state = configuration_store_ && !configuration_store_->initialized() ? "initializing" : "ready",
        .configuration_state = configuration_store_ && configuration_store_->initialized() ? "ready" : "pending",
        .backup_root = configuration_store_ ? configuration_store_->current().backup_root.string() : std::string{},
        .auto_edit_save_enabled = configuration_store_ ? configuration_store_->current().auto_edit_save_enabled : true,
        .auto_edit_save_interval_seconds = configuration_store_ ? configuration_store_->current().auto_edit_save_interval_seconds : 30,
        .recovery_available = recovery_available,
    };
}

} // namespace ddse::application
