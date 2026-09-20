#pragma once

#include <map>
#include <string>
#include <string_view>

namespace ddse::application {

enum class LogLevel { Trace, Debug, Info, Warn, Error };
using LogFields = std::map<std::string, std::string>;

class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level, std::string_view message, const LogFields& fields = {}) = 0;
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

} // namespace ddse::application
