#include <kin/assets/image.hpp>
#include <kin/assets/content.hpp>
#include <kin/renderer/sprite_catalog.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/tilemap/tilemap.hpp>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

void validate(const std::filesystem::path& root) {
#if KIN_ENABLE_IMAGE_IO
    assert(kin::load_image(root / "images/hero.png").valid());
#endif
    auto sprites = kin::load_sprite_catalog(root / "sprites/hero.kinsprites");
    assert(sprites.sprite_ids().size() == 2);
    const auto* walk = sprites.animation("hero.walk");
    assert(walk && walk->frames[0].sprite == "hero.1");
    assert(std::abs(kin::sprite_animation_length(*walk) - 0.2f) < 0.00001f);
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    for (const char* name : {"tiled", "ldtk"}) {
        std::string error;
        auto map = kin::load_tilemap(root / (std::string("maps/") + name + ".kintilemap"), root, &error);
        if (!error.empty()) std::cerr << error << '\n';
        assert(error.empty() && map.cols == 2 && map.rows == 1);
        assert(map.tileset.sheet.tile_w == 2 && map.tileset.sheet.tile_h == 2);
        assert(map.tileset.tile(2) && map.tileset.tile(2)->frame == 1);
        auto scene = kin::load_scene_asset(root / (std::string("maps/") + name + ".kinscene"), world.components());
        for (const auto& problem : scene.diagnostics) std::cerr << problem << '\n';
        assert(scene.ok());
    }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path root{argv[1]};
    validate(root);
    const auto file = root.parent_path() / "generated.kinpak";
    const auto result = kin::write_content_pack(root, file);
    if (!result.ok) std::cerr << result.error << '\n';
    assert(result.ok);
    const auto pack = kin::ContentPack::open(file);
    assert(pack && pack->verify() && !pack->find(".kin-import-report.json"));
    const auto mounted = root.parent_path() / "mounted";
    kin::mount_content_pack(mounted, pack);
    validate(mounted);
    kin::unmount_content_pack(mounted);
}
