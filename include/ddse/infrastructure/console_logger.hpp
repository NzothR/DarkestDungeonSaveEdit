#pragma once

#include "ddse/application/logger.hpp"

#include <iosfwd>
#include <mutex>

namespace ddse::infrastructure {

class ConsoleLogger final : public application::ILogger {
public:
    explicit ConsoleLogger(std::ostream& output);
    void log(application::LogLevel level, std::string_view message,
             const application::LogFields& fields = {}) override;

private:
    std::ostream& output_;
    std::mutex mutex_;
};

} // namespace ddse::infrastructure
