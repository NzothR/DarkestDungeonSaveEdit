#include "ddse/application/application_info.hpp"

#include "ddse/core/version.hpp"

namespace ddse::application {
std::string description() { return "DDSE " + std::string{core::version()}; }
}
