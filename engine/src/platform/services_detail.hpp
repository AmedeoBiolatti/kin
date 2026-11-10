#pragma once

#include <kin/platform/services.hpp>

namespace kin::services_detail {

// The Steam backend (services_steam.cpp): null when the Steam API library is
// not there or Steam does not answer, with `start` left Offline, or set to
// Restarting when Steam starts the game again itself.
std::unique_ptr<PlatformServices> start_steam(const ServicesConfig& config, ServicesStart& start);

} // namespace kin::services_detail
