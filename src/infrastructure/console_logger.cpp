#include "ddse/infrastructure/console_logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <ostream>

namespace ddse::infrastructure {

ConsoleLogger::ConsoleLogger(std::ostream& output) : output_(output) {}

void ConsoleLogger::log(application::LogLevel level, std::string_view message,
                        const application::LogFields& fields) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::lock_guard lock{mutex_};
    output_ << std::put_time(&local, "%Y-%m-%dT%H:%M:%S") << ' '
            << application::to_string(level) << " message=\"" << message << '"';
    for (const auto& [key, value] : fields) {
        output_ << ' ' << key << "=\"" << value << '"';
    }
    output_ << '\n';
}

} // namespace ddse::infrastructure
