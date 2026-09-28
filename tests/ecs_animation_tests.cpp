#include <kin/anim/player.hpp>
#include <kin/ecs/render.hpp>

#include <cassert>
#include <memory>
#include <utility>

namespace {

class FakeTextureBackend final : public kin::ITextureBackend {
public:
    explicit FakeTextureBackend(kin::Vec2i size)
        : _size(size) {
    }

    kin::Vec2i size() const override { return _size; }

private:
    kin::Vec2i _size{};
};

kin::Texture make_texture(kin::Vec2i size = {64, 16}) {
    return kin::Texture{std::make_shared<FakeTextureBackend>(size)};
}

kin::SpriteCatalog make_catalog(kin::Texture texture) {
    kin::SpriteCatalog catalog;
    catalog.set_texture("actors", std::move(texture));
    catalog.add({.id = "hero.0", .texture_id = "actors", .source = {0.0f, 0.0f, 8.0f, 8.0f}});
    catalog.add({.id = "hero.1", .texture_id = "actors", .source = {8.0f, 0.0f, 8.0f, 8.0f}});
    catalog.add({.id = "hero.2", .texture_id = "actors", .source = {16.0f, 0.0f, 8.0f, 8.0f}});
    return catalog;
}

void register_components(kin::EcsWorld& world) {
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    world.component<kin::AnimationEventQueue>("AnimationEventQueue");
    world.component<kin::SpriteRenderer>("SpriteRenderer");
}

kin::Animation make_sprite_animation(std::string name, kin::Clip clip, bool loop) {
    return kin::Animation{
        .name = std::move(name),
        .root = loop ? kin::repeat_node(kin::clip_node(std::move(clip)), 0) : kin::clip_node(std::move(clip)),
    };
}

kin::AnimationPlayer make_player(kin::AnimationRegistry& registry,
                                 const kin::SpriteCatalog& catalog,
                                 std::string_view animation) {
    kin::AnimationPlayer player{
        .registry = &registry,
        .sprite_catalog = &catalog,
    };
    kin::set_base(player, animation);
    return player;
}

void test_looping_animation_updates_sprite_renderer() {
    kin::SpriteCatalog catalog = make_catalog(make_texture());
    kin::SpriteTrack sprites{.keys = {
        {.time = 0.0f, .sprite_id = "hero.0"},
        {.time = 0.1f, .sprite_id = "hero.1"},
    }};

    kin::AnimationRegistry registry;
    registry.add(make_sprite_animation("hero.loop", kin::Clip{.duration = 0.2f, .sprites = {std::move(sprites)}}, true));

    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity entity = world.entity("animated")
                                .set(make_player(registry, catalog, "hero.loop"))
                                .set(kin::SpriteRenderer{});

    kin::sample_animation_players(world);
    assert(entity.get<kin::SpriteRenderer>()->sprite.id == "hero.0");

    kin::advance_animation_players(world, 0.1f);
    kin::sample_animation_players(world);
    assert(entity.get<kin::SpriteRenderer>()->sprite.id == "hero.1");

    kin::advance_animation_players(world, 0.1f);
    kin::sample_animation_players(world);
    assert(entity.get<kin::SpriteRenderer>()->sprite.id == "hero.0");
}

void test_non_looping_animation_finishes_on_last_frame() {
    kin::SpriteCatalog catalog = make_catalog(make_texture());
    kin::SpriteTrack sprites{.keys = {
        {.time = 0.0f, .sprite_id = "hero.0"},
        {.time = 0.1f, .sprite_id = "hero.1"},
        {.time = 0.2f, .sprite_id = "hero.2"},
    }};

    kin::AnimationRegistry registry;
    registry.add(make_sprite_animation("hero.once", kin::Clip{.duration = 0.2f, .sprites = {std::move(sprites)}}, false));

    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity entity = world.entity("animated")
                                .set(make_player(registry, catalog, "hero.once"))
                                .set(kin::SpriteRenderer{});

    kin::advance_animation_players(world, 1.0f);
    kin::sample_animation_players(world);

    const auto* player = entity.get<kin::AnimationPlayer>();
    assert(player != nullptr);
    assert(player->layers.front().cursor.finished);
    assert(entity.get<kin::SpriteRenderer>()->sprite.id == "hero.2");
}

void test_animation_player_emits_event_queue() {
    kin::Clip clip{
        .duration = 0.1f,
        .events = {{
            .keys = {{
                .time = 0.1f,
                .event = {.channel = "sound", .name = "step", .value = "step"},
            }},
        }},
    };

    kin::AnimationRegistry registry;
    registry.add(kin::Animation{.name = "event.once", .root = kin::clip_node(std::move(clip))});
    kin::SpriteCatalog catalog = make_catalog(make_texture());

    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity entity = world.entity("animated")
                                .set(make_player(registry, catalog, "event.once"))
                                .set(kin::SpriteRenderer{});

    kin::advance_animation_players(world, 0.1f);

    const auto* queue = entity.get<kin::AnimationEventQueue>();
    assert(queue != nullptr);
    assert(queue->pending.size() == 1);
    assert(queue->pending[0].channel == "sound");
    assert(queue->pending[0].value == "step");
}

} // namespace

int main() {
    test_looping_animation_updates_sprite_renderer();
    test_non_looping_animation_finishes_on_last_frame();
    test_animation_player_emits_event_queue();
    return 0;
}
