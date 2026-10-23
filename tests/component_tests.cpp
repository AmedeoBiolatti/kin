#include <kin/components.hpp>

static_assert(kin::components::audio == bool(KIN_ENABLE_AUDIO));
static_assert(kin::components::physics == bool(KIN_ENABLE_PHYSICS));
static_assert(kin::components::scripting == bool(KIN_ENABLE_SCRIPTING));
static_assert(kin::components::ttf == bool(KIN_ENABLE_TTF));
static_assert(kin::components::image_io == bool(KIN_ENABLE_IMAGE_IO));
static_assert(kin::components::scene_server == bool(KIN_ENABLE_SCENE_SERVER));
static_assert(kin::components::debug_overlay == bool(KIN_ENABLE_DEBUG_OVERLAY));
static_assert(kin::components::runtime_profiling == bool(KIN_ENABLE_RUNTIME_PROFILING));
static_assert(kin::components::runtime_assets == bool(KIN_ENABLE_RUNTIME_ASSETS));
#ifdef KIN_ENABLE_RENDER_PROBE
static_assert(KIN_HAS_RENDER_PROBE == 1);
#else
static_assert(KIN_HAS_RENDER_PROBE == 0);
#endif
#ifdef KIN_ENABLE_DETERMINISM_CHECK
static_assert(KIN_HAS_DETERMINISM_CHECK == 1);
#else
static_assert(KIN_HAS_DETERMINISM_CHECK == 0);
#endif
int main() {}
