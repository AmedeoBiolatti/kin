#include <kin/platform/services.hpp>

// In kin_foundation, so that kin_scripting's `services` table reaches the
// active services without linking kin_engine.

namespace kin {
namespace {

PlatformServices* g_active = nullptr;

} // namespace

PlatformServices* active_services() {
    return g_active;
}

void set_active_services(PlatformServices* services) {
    g_active = services;
}

} // namespace kin
