#include <kin/ecs/render.hpp>
#include <kin/ecs/sprite_animator.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/sprite_catalog.hpp>
#include <kin/scene/scene_asset.hpp>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

bool near(kin::f32 a, kin::f32 b) {
    return std::abs(a - b) < 1e-4f;
}

kin::SpriteCatalog walker() {
    kin::SpriteCatalog catalog;
    catalog.set_texture("hero", "images/hero.png", {});
    catalog.add_sheet({.id = "hero", .texture_id = "hero", .grid = {.tile_w = 16, .tile_h = 24}});
    for (int frame = 0; frame < 4; ++frame) {
        catalog.add_sheet_sprite("hero." + std::to_string(frame), "hero", frame, {}, {0.5f, 1.0f});
    }
    catalog.add_animation({.id = "walk",
                           .frames = {{.sprite = "hero.0"}, {.sprite = "hero.1", .hold = 2.0f}, {.sprite = "hero.2"}},
                           .fps = 10.0f});
    catalog.add_animation({.id = "jump",
                           .frames = {{.sprite = "hero.3"}, {.sprite = "hero.0"}},
                           .fps = 5.0f,
                           .playback = kin::SpritePlayback::Once});
    catalog.add_animation({.id = "sway",
                           .frames = {{.sprite = "hero.0"}, {.sprite = "hero.1"}, {.sprite = "hero.2"}},
                           .fps = 10.0f,
                           .playback = kin::SpritePlayback::PingPong});
    return catalog;
}

void test_catalog_animations_read_and_written() {
    // Written as `animation` lines after the sprites, read back the same.
    const kin::SpriteCatalog catalog = walker();
    const std::string text = kin::serialize_sprite_catalog(catalog);
    assert(text.find("animation jump once 5 hero.3 hero.0\n") != std::string::npos);
    assert(text.find("animation sway pingpong 10 hero.0 hero.1 hero.2\n") != std::string::npos);
    assert(text.find("animation walk loop 10 hero.0 hero.1*2 hero.2\n") != std::string::npos);
    assert(text.find("sheet_sprite hero.3") < text.find("animation jump"));

    kin::SpriteCatalog read;
    std::string error;
    assert(kin::parse_sprite_catalog(text, read, error) && error.empty());
    assert(read.animation_ids() == (std::vector<std::string>{"jump", "sway", "walk"}));
    const kin::SpriteAnimationDefinition* walk = read.animation("walk");
    assert(walk && walk->frames.size() == 3 && walk->frames[1].sprite == "hero.1" && near(walk->frames[1].hold, 2.0f));
    assert(walk->playback == kin::SpritePlayback::Loop && near(walk->fps, 10.0f));
    assert(read.animation("jump")->playback == kin::SpritePlayback::Once);
    assert(read.sprite("hero.2")->frame == 2 && near(read.sprite("hero.2")->pivot.y, 1.0f));
    assert(kin::serialize_sprite_catalog(read) == text);

    // Saved and loaded as a file (an animation without frames too).
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "kin_sprite_animation_test.kinsprites";
    kin::SpriteCatalog saved = catalog;
    saved.add_animation({.id = "empty"});
    assert(kin::save_sprite_catalog(saved, file));
    const kin::SpriteCatalog loaded = kin::load_sprite_catalog(file);
    assert(loaded.animation("empty") && loaded.animation("empty")->frames.empty() && loaded.animations().size() == 4);

    // What does not read: its line, and why.
    const auto fails = [](std::string_view bad, std::string_view why) {
        kin::SpriteCatalog out;
        std::string error;
        const bool read = kin::parse_sprite_catalog(bad, out, error);
        return !read && error.find(why) != std::string::npos;
    };
    assert(fails("texture a a.png\nanimation walk sideways 10 a\n", "line 2: malformed animation"));
    assert(fails("animation walk loop 0 a\n", "malformed animation"));
    assert(fails("animation walk loop 10 a*x\n", "malformed animation frame 'a*x'"));
    assert(fails("animation walk loop 10 a*-1\n", "malformed animation frame"));
    assert(fails("animation walk loop 10 *2\n", "malformed animation frame"));
    assert(fails("frame a\n", "line 1: unknown directive"));
    // One with no sprites reads; kin's loader refuses it (it shows nothing).
    kin::SpriteCatalog none;
    assert(kin::parse_sprite_catalog("# nothing yet\n", none, error) && none.sprite_ids().empty());
    std::ofstream{file} << "# nothing yet\n";
    bool refused = false;
    try {
        (void)kin::load_sprite_catalog(file);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    assert(refused);
    std::ofstream{file} << "animation walk loop 10 a\nsheet\n";
    try {
        (void)kin::load_sprite_catalog(file);
        assert(false);
    } catch (const std::runtime_error& failure) {
        assert(std::string(failure.what()).ends_with(".kinsprites:2: malformed sheet"));
    }
    fs::remove(file);

    // Removed, by kind.
    kin::SpriteCatalog edited = catalog;
    assert(edited.remove_animation("walk") && !edited.remove_animation("walk") && !edited.animation("walk"));
    assert(edited.remove("hero.3") && !edited.contains("hero.3"));
    assert(edited.remove_sheet("hero") && !edited.sheet("hero"));
    assert(edited.remove_texture("hero") && !edited.texture("hero"));
    edited.clear();
    assert(edited.animations().empty());
}

void test_animation_frames_in_time() {
    const kin::SpriteCatalog catalog = walker();
    // Loop: 0.1 s, 0.2 s (held for two), 0.1 s; then again.
    const kin::SpriteAnimationDefinition& walk = *catalog.animation("walk");
    assert(near(kin::sprite_animation_length(walk), 0.4f));
    bool finished = true;
    assert(kin::sprite_animation_frame(walk, 0.0f, &finished) == 0 && !finished);
    assert(kin::sprite_animation_frame(walk, 0.05f) == 0);
    assert(kin::sprite_animation_frame(walk, 0.15f) == 1);
    assert(kin::sprite_animation_frame(walk, 0.29f) == 1);
    assert(kin::sprite_animation_frame(walk, 0.31f) == 2);
    assert(kin::sprite_animation_frame(walk, 0.41f) == 0);
    assert(kin::sprite_animation_frame(walk, 4.15f) == 1);
    assert(kin::sprite_animation_frame(walk, -0.05f) == 2); // backwards, from its end

    // Once: its last frame stays, and it says so.
    const kin::SpriteAnimationDefinition& jump = *catalog.animation("jump");
    assert(near(kin::sprite_animation_length(jump), 0.4f));
    assert(kin::sprite_animation_frame(jump, 0.25f, &finished) == 1 && !finished);
    assert(kin::sprite_animation_frame(jump, 0.4f, &finished) == 1 && finished);
    assert(kin::sprite_animation_frame(jump, 9.0f, &finished) == 1 && finished);
    assert(kin::sprite_animation_frame(jump, -1.0f, &finished) == 0 && !finished);

    // PingPong: 0 1 2 1, 0 1 2 1, ...
    const kin::SpriteAnimationDefinition& sway = *catalog.animation("sway");
    assert(near(kin::sprite_animation_length(sway), 0.4f));
    const int expected[] = {0, 1, 2, 1, 0, 1, 2, 1};
    for (int step = 0; step < 8; ++step) {
        assert(kin::sprite_animation_frame(sway, 0.05f + 0.1f * static_cast<float>(step), &finished) == expected[step] && !finished);
    }
    kin::SpriteAnimationDefinition two{.id = "two", .frames = {{.sprite = "a"}, {.sprite = "b"}}, .fps = 10.0f,
                                       .playback = kin::SpritePlayback::PingPong};
    assert(near(kin::sprite_animation_length(two), 0.2f));
    assert(kin::sprite_animation_frame(two, 0.15f) == 1 && kin::sprite_animation_frame(two, 0.25f) == 0);

    // Without frames: none.
    const kin::SpriteAnimationDefinition empty{.id = "empty"};
    assert(kin::sprite_animation_length(empty) == 0.0f && kin::sprite_animation_frame(empty, 1.0f) == -1);
}

void test_animators_set_their_sprites() {
    kin::SpriteCatalog catalog = walker();
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world); // as scenes have them: described, inherited by instances
    const kin::ComponentDescriptor* described = world.components().find("SpriteAnimator");
    assert(described);
    const auto field = [&](std::string_view name) {
        for (const kin::ComponentFieldDescriptor& f : described->fields) {
            if (f.name == name) {
                return &f;
            }
        }
        return static_cast<const kin::ComponentFieldDescriptor*>(nullptr);
    };
    assert(field("animation") && field("animation")->entry_of == "SpriteRenderer.asset");
    assert(field("frame") && field("frame")->readonly && field("speed") && field("playing"));

    kin::EcsEntity hero = world.entity()
                              .set(kin::SpriteRenderer{.sprite = catalog.ref("hero.3")})
                              .set(kin::SpriteAnimator{.animation = "walk"});
    kin::update_sprite_animators(world, 0.0f);
    assert(hero.get<kin::SpriteAnimator>()->frame == 0 && hero.get<kin::SpriteRenderer>()->sprite.id == "hero.0");
    kin::update_sprite_animators(world, 0.15f);
    assert(hero.get<kin::SpriteAnimator>()->frame == 1 && hero.get<kin::SpriteRenderer>()->sprite.id == "hero.1");
    assert(hero.get<kin::SpriteRenderer>()->sprite.catalog == &catalog);
    // Held: it stays; twice as fast: on.
    hero.get_mut<kin::SpriteAnimator>()->playing = false;
    kin::update_sprite_animators(world, 1.0f);
    assert(hero.get<kin::SpriteAnimator>()->frame == 1);
    hero.get_mut<kin::SpriteAnimator>()->playing = true;
    hero.get_mut<kin::SpriteAnimator>()->speed = 2.0f;
    kin::update_sprite_animators(world, 0.1f); // 0.15 + 0.2
    assert(hero.get<kin::SpriteAnimator>()->frame == 2 && hero.get<kin::SpriteRenderer>()->sprite.id == "hero.2");
    // Time kept within a turn as it plays on.
    kin::update_sprite_animators(world, 100.0f);
    assert(hero.get<kin::SpriteAnimator>()->time < 0.4f);

    // Another named: from its start. A Once one plays through, and stays.
    hero.get_mut<kin::SpriteAnimator>()->speed = 1.0f;
    hero.get_mut<kin::SpriteAnimator>()->animation = "jump";
    kin::update_sprite_animators(world, 0.1f);
    assert(near(hero.get<kin::SpriteAnimator>()->time, 0.1f) && hero.get<kin::SpriteRenderer>()->sprite.id == "hero.3");
    kin::update_sprite_animators(world, 0.5f);
    assert(hero.get<kin::SpriteAnimator>()->finished && hero.get<kin::SpriteRenderer>()->sprite.id == "hero.0");
    kin::update_sprite_animators(world, 5.0f);
    assert(hero.get<kin::SpriteAnimator>()->finished && hero.get<kin::SpriteAnimator>()->frame == 1);
    hero.get_mut<kin::SpriteAnimator>()->time = 0.0f; // again
    kin::update_sprite_animators(world, 0.0f);
    assert(!hero.get<kin::SpriteAnimator>()->finished && hero.get<kin::SpriteRenderer>()->sprite.id == "hero.3");

    // No such animation, or a catalog not bound yet: the sprite stays.
    hero.get_mut<kin::SpriteAnimator>()->animation = "fly";
    kin::update_sprite_animators(world, 0.1f);
    assert(hero.get<kin::SpriteAnimator>()->frame == -1 && hero.get<kin::SpriteRenderer>()->sprite.id == "hero.3");
    kin::EcsEntity unbound = world.entity().set(kin::SpriteRenderer{}).set(kin::SpriteAnimator{.animation = "walk"});
    kin::update_sprite_animators(world, 0.1f);
    assert(unbound.get<kin::SpriteAnimator>()->frame == -1 && unbound.get<kin::SpriteRenderer>()->sprite.id.empty());

    // Instances of a prefab: each gets its own to move on; the prefab's stay.
    flecs::entity prefab = world.raw().prefab("walker");
    prefab.set(kin::SpriteRenderer{.sprite = catalog.ref("hero.0")}).set(kin::SpriteAnimator{.animation = "walk"});
    flecs::entity a = world.raw().entity().is_a(prefab);
    flecs::entity b = world.raw().entity().is_a(prefab);
    assert(!a.owns<kin::SpriteAnimator>());
    kin::update_sprite_animators(world, 0.15f);
    assert(a.owns<kin::SpriteAnimator>() && a.owns<kin::SpriteRenderer>() && b.owns<kin::SpriteAnimator>());
    assert(a.get<kin::SpriteRenderer>()->sprite.id == "hero.1" && b.get<kin::SpriteAnimator>()->frame == 1);
    assert(prefab.get<kin::SpriteAnimator>()->time == 0.0f && prefab.get<kin::SpriteRenderer>()->sprite.id == "hero.0");
    a.get_mut<kin::SpriteAnimator>()->animation = "jump";
    kin::update_sprite_animators(world, 0.1f);
    assert(a.get<kin::SpriteRenderer>()->sprite.id == "hero.3" && b.get<kin::SpriteRenderer>()->sprite.id == "hero.1");
}

} // namespace

int main() {
    test_catalog_animations_read_and_written();
    test_animation_frames_in_time();
    test_animators_set_their_sprites();
    return 0;
}
