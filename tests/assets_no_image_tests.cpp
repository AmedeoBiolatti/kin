#include <kin/assets/asset_manager.hpp>
#include <kin/assets/image.hpp>
#include <cassert>
#include <stdexcept>

void run_anim_asset_loader_tests();

int main() {
    bool refused = false;
    try { kin::load_image("unused.png"); }
    catch (const std::runtime_error&) { refused = true; }
    assert(refused);
    kin::Image image{{1, 1}, {255, 0, 0, 255}};
    assert(image.valid());
    assert(!kin::save_image(image, "unused.png"));
    // Native assets still work when file codecs are disabled.
    run_anim_asset_loader_tests();
}
