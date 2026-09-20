#include "ddse/application/logger.hpp"

namespace ddse::application {
std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}
} // namespace ddse::application
