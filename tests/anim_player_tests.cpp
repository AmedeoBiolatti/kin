#include <kin/anim/binding.hpp>
#include <kin/anim/player.hpp>
#include <kin/anim/preview.hpp>
#include <kin/ecs/render.hpp>

#include <cassert>
#include <cmath>
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

struct TestAnimProperties {
    kin::i32 count = 0;
    bool visible = false;
    std::string label;
};

bool near(kin::f32 a, kin::f32 b) {
    return std::fabs(a - b) < 0.0001f;
}

kin::Texture make_texture(kin::Vec2i size = {32, 16}) {
    return kin::Texture{std::make_shared<FakeTextureBackend>(size)};
}

kin::SpriteCatalog make_catalog() {
    kin::SpriteCatalog catalog;
    catalog.set_texture("main", make_texture());
    catalog.add({.id = "hero.0", .texture_id = "main", .source = {0.0f, 0.0f, 8.0f, 8.0f}});
    catalog.add({.id = "hero.1", .texture_id = "main", .source = {8.0f, 0.0f, 8.0f, 8.0f}});
    return catalog;
}

void register_components(kin::EcsWorld& world) {
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::SpriteRenderer>("SpriteRenderer");
    world.component<kin::TextureRenderer>("TextureRenderer");
    world.component<kin::RectRenderer>("RectRenderer");
    world.component<kin::LineRenderer>("LineRenderer");
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    world.component<TestAnimProperties>("TestAnimProperties");
}

kin::Animation make_animation(std::string name, kin::Clip clip, bool loop = true) {
    return kin::Animation{
        .name = std::move(name),
        .root = loop ? kin::repeat_node(kin::clip_node(std::move(clip)), 0) : kin::clip_node(std::move(clip)),
    };
}

kin::PropertyTrack vec_track(std::string property, kin::Vec2f a, kin::Vec2f b, kin::f32 duration) {
    return kin::PropertyTrack{
        .property = std::move(property),
        .keys = {
            {.time = 0.0f, .value = a},
            {.time = duration, .value = b},
        },
    };
}

kin::PropertyTrack float_track(std::string property, kin::f32 a, kin::f32 b, kin::f32 duration) {
    return kin::PropertyTrack{
        .property = std::move(property),
        .keys = {
            {.time = 0.0f, .value = a},
            {.time = duration, .value = b},
        },
    };
}

kin::PropertyTrack value_track(std::string property, kin::AnimValue value) {
    return kin::PropertyTrack{
        .property = std::move(property),
        .keys = {
            {.time = 0.0f, .value = std::move(value)},
        },
    };
}

kin::AnimationPlayer make_player(kin::AnimationRegistry& animations,
                                 const kin::PropertyRegistry& properties,
                                 const kin::SpriteCatalog* catalog,
                                 std::string_view animation) {
    kin::AnimationPlayer player{
        .registry = &animations,
        .properties = &properties,
        .sprite_catalog = catalog,
    };
    kin::set_base(player, animation);
    return player;
}

void test_builtin_property_binding() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    const kin::PropertyAccessor* offset = properties.find("SpriteRenderer.offset");
    assert(offset != nullptr);
    assert(offset->kind == kin::AnimValueKind::Vec2);

    kin::EcsEntity entity = world.entity("sprite").set(kin::SpriteRenderer{});
    auto* renderer = entity.get_mut<kin::SpriteRenderer>();
    assert(renderer != nullptr);

    offset->set(renderer, kin::Vec2f{3.0f, 4.0f});
    assert((renderer->offset == kin::Vec2f{3.0f, 4.0f}));
    assert((std::get<kin::Vec2f>(offset->get(renderer)) == kin::Vec2f{3.0f, 4.0f}));

    const kin::PropertyAccessor* tint = properties.find("SpriteRenderer.tint");
    assert(tint != nullptr);
    assert(tint->kind == kin::AnimValueKind::Color);
}

void test_looping_property_track_drives_sprite_offset() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::PropertyTrack offset_track;
    offset_track.property = "SpriteRenderer.offset";
    offset_track.keys = {
        {.time = 0.0f, .value = kin::Vec2f{0.0f, 0.0f}},
        {.time = 0.5f, .value = kin::Vec2f{10.0f, 20.0f}},
    };

    kin::AnimationRegistry animations;
    animations.add(make_animation("offset.loop", kin::Clip{.duration = 0.5f, .properties = {std::move(offset_track)}}));

    kin::EcsEntity entity = world.entity("animated")
                                .set(kin::SpriteRenderer{})
                                .set(make_player(animations, properties, nullptr, "offset.loop"));

    kin::advance_animation_players(world, 0.25f);
    kin::sample_animation_players(world);

    const auto* renderer = entity.get<kin::SpriteRenderer>();
    assert(renderer != nullptr);
    assert(near(renderer->offset.x, 5.0f));
    assert(near(renderer->offset.y, 10.0f));

    kin::advance_animation_players(world, 0.25f);
    kin::advance_animation_players(world, 0.25f);
    kin::sample_animation_players(world);

    renderer = entity.get<kin::SpriteRenderer>();
    assert(near(renderer->offset.x, 5.0f));
    assert(near(renderer->offset.y, 10.0f));
}

void test_relative_track_captures_per_entity_start_value_once() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::PropertyTrack relative;
    relative.property = "SpriteRenderer.offset";
    relative.space = kin::TrackSpace::Relative;
    relative.keys = {
        {.time = 0.0f, .value = kin::Vec2f{0.0f, 0.0f}},
        {.time = 1.0f, .value = kin::Vec2f{2.0f, 3.0f}},
    };

    kin::AnimationRegistry animations;
    animations.add(make_animation("relative.once", kin::Clip{.duration = 1.0f, .properties = {std::move(relative)}}, false));

    kin::EcsEntity first = world.entity("first")
                               .set(kin::SpriteRenderer{.offset = {10.0f, 20.0f}})
                               .set(make_player(animations, properties, nullptr, "relative.once"));
    kin::EcsEntity second = world.entity("second")
                                .set(kin::SpriteRenderer{.offset = {-5.0f, 1.0f}})
                                .set(make_player(animations, properties, nullptr, "relative.once"));

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);

    assert((first.get<kin::SpriteRenderer>()->offset == kin::Vec2f{11.0f, 21.5f}));
    assert((second.get<kin::SpriteRenderer>()->offset == kin::Vec2f{-4.0f, 2.5f}));

    first.get_mut<kin::SpriteRenderer>()->offset = {100.0f, 100.0f};
    kin::advance_animation_players(world, 0.25f);
    kin::sample_animation_players(world);
    assert((first.get<kin::SpriteRenderer>()->offset == kin::Vec2f{11.5f, 22.25f}));
}

void test_sprite_track_sets_sprite_and_key_pivot() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);
    kin::SpriteCatalog catalog = make_catalog();

    kin::SpriteTrack sprites;
    sprites.keys = {
        {.time = 0.0f, .sprite_id = "hero.0"},
        {.time = 0.2f, .sprite_id = "hero.1", .pivot = {0.25f, 0.75f}, .has_pivot = true},
    };

    kin::AnimationRegistry animations;
    animations.add(make_animation("sprite.loop", kin::Clip{.duration = 0.4f, .sprites = {std::move(sprites)}}, true));

    kin::EcsEntity entity = world.entity("sprite")
                                .set(kin::SpriteRenderer{})
                                .set(make_player(animations, properties, &catalog, "sprite.loop"));

    kin::sample_animation_players(world);
    const auto* renderer = entity.get<kin::SpriteRenderer>();
    assert(renderer != nullptr);
    assert(renderer->sprite.catalog == &catalog);
    assert(renderer->sprite.id == "hero.0");

    kin::advance_animation_players(world, 0.2f);
    kin::sample_animation_players(world);

    renderer = entity.get<kin::SpriteRenderer>();
    assert(renderer->sprite.catalog == &catalog);
    assert(renderer->sprite.id == "hero.1");
    assert((renderer->pivot == kin::Vec2f{0.25f, 0.75f}));
}

void test_sprite_track_preserves_existing_catalog_when_player_has_none() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);
    kin::SpriteCatalog catalog = make_catalog();

    kin::SpriteTrack sprites;
    sprites.keys = {{.time = 0.0f, .sprite_id = "hero.1"}};

    kin::AnimationRegistry animations;
    animations.add(make_animation("sprite.fallback", kin::Clip{.duration = 0.2f, .sprites = {std::move(sprites)}}, true));

    kin::EcsEntity entity = world.entity("sprite")
                                .set(kin::SpriteRenderer{.sprite = catalog.ref("hero.0")})
                                .set(make_player(animations, properties, nullptr, "sprite.fallback"));

    kin::sample_animation_players(world);
    const auto* renderer = entity.get<kin::SpriteRenderer>();
    assert(renderer != nullptr);
    assert(renderer->sprite.catalog == &catalog);
    assert(renderer->sprite.id == "hero.1");
}

void test_sequence_advances_with_dt_carryover() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    std::vector<kin::AnimationNode> sequence_children;
    sequence_children.push_back(kin::clip_node(kin::Clip{.duration = 0.2f, .properties = {vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {10.0f, 0.0f}, 0.2f)}}));
    sequence_children.push_back(kin::clip_node(kin::Clip{.duration = 0.4f, .properties = {vec_track("SpriteRenderer.offset", {10.0f, 0.0f}, {30.0f, 0.0f}, 0.4f)}}));
    kin::Animation animation{
        .name = "sequence",
        .root = kin::sequence_node(std::move(sequence_children)),
    };

    kin::AnimationRegistry animations;
    animations.add(std::move(animation));

    kin::EcsEntity entity = world.entity("sequence")
                                .set(kin::SpriteRenderer{})
                                .set(make_player(animations, properties, nullptr, "sequence"));

    kin::advance_animation_players(world, 0.3f);
    kin::sample_animation_players(world);
    assert(near(entity.get<kin::SpriteRenderer>()->offset.x, 15.0f));
}

void test_parallel_end_policies() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    auto make_parallel = [](std::string name, kin::ParallelEnd end, kin::i32 primary = 0) {
        std::vector<kin::AnimationNode> children;
        children.push_back(kin::clip_node(kin::Clip{.duration = 0.2f, .properties = {float_track("SpriteRenderer.rotation", 0.0f, 20.0f, 0.2f)}}));
        children.push_back(kin::clip_node(kin::Clip{.duration = 0.5f, .properties = {vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {50.0f, 0.0f}, 0.5f)}}));
        kin::AnimationNode node = kin::parallel_node(std::move(children), end);
        std::get<kin::Parallel>(node.value).primary = primary;
        return kin::Animation{.name = std::move(name), .root = std::move(node)};
    };

    kin::AnimationRegistry animations;
    animations.add(make_parallel("parallel.all", kin::ParallelEnd::All));
    animations.add(make_parallel("parallel.any", kin::ParallelEnd::Any));
    animations.add(make_parallel("parallel.primary", kin::ParallelEnd::Primary, 0));

    auto add_entity = [&](std::string_view name, std::string_view animation) {
        return world.entity(name)
            .set(kin::SpriteRenderer{})
            .set(make_player(animations, properties, nullptr, animation));
    };

    kin::EcsEntity all = add_entity("all", "parallel.all");
    kin::EcsEntity any = add_entity("any", "parallel.any");
    kin::EcsEntity primary = add_entity("primary", "parallel.primary");

    kin::advance_animation_players(world, 0.25f);
    assert(!all.get<kin::AnimationPlayer>()->layers.front().cursor.finished);
    assert(any.get<kin::AnimationPlayer>()->layers.front().cursor.finished);
    assert(primary.get<kin::AnimationPlayer>()->layers.front().cursor.finished);
}

void test_finite_repeat_finishes_and_infinite_repeat_keeps_looping() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::Clip clip{.duration = 0.2f, .properties = {vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {10.0f, 0.0f}, 0.2f)}};
    kin::AnimationRegistry animations;
    kin::Clip finite_clip = clip;
    animations.add(kin::Animation{.name = "repeat.finite", .root = kin::repeat_node(kin::clip_node(std::move(finite_clip)), 2)});
    animations.add(make_animation("repeat.infinite", std::move(clip), true));

    kin::EcsEntity finite = world.entity("finite")
                                .set(kin::SpriteRenderer{})
                                .set(make_player(animations, properties, nullptr, "repeat.finite"));
    kin::EcsEntity infinite = world.entity("infinite")
                                  .set(kin::SpriteRenderer{})
                                  .set(make_player(animations, properties, nullptr, "repeat.infinite"));

    kin::advance_animation_players(world, 0.45f);
    assert(finite.get<kin::AnimationPlayer>()->layers.front().cursor.finished);
    assert(!infinite.get<kin::AnimationPlayer>()->layers.front().cursor.finished);
}

void test_ref_resolves_and_cycle_does_not_hang() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::AnimationRegistry animations;
    animations.add(make_animation("target", kin::Clip{.duration = 1.0f, .properties = {vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {10.0f, 0.0f}, 1.0f)}}, false));
    animations.add(kin::Animation{.name = "ref", .root = kin::ref_node("target")});
    animations.add(kin::Animation{.name = "cycle.a", .root = kin::ref_node("cycle.b")});
    animations.add(kin::Animation{.name = "cycle.b", .root = kin::ref_node("cycle.a")});

    kin::EcsEntity ref = world.entity("ref")
                             .set(kin::SpriteRenderer{})
                             .set(make_player(animations, properties, nullptr, "ref"));
    kin::EcsEntity cycle = world.entity("cycle")
                               .set(kin::SpriteRenderer{.offset = {7.0f, 8.0f}})
                               .set(make_player(animations, properties, nullptr, "cycle.a"));

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);
    assert(near(ref.get<kin::SpriteRenderer>()->offset.x, 5.0f));
    assert((cycle.get<kin::SpriteRenderer>()->offset == kin::Vec2f{7.0f, 8.0f}));
}

void test_runtime_ref_names_resolve_through_player_bindings() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::AnimationRegistry animations;
    animations.add(make_animation("hero.child", kin::Clip{.duration = 1.0f, .properties = {float_track("SpriteRenderer.rotation", 0.0f, 20.0f, 1.0f)}}, true));
    animations.add(kin::Animation{.name = "bound.parent", .root = kin::ref_node("{X}.child")});

    kin::AnimationPlayer player = make_player(animations, properties, nullptr, "bound.parent");
    player.bindings = {{"X", "hero"}};
    kin::EcsEntity entity = world.entity("bound-ref")
                                .set(kin::SpriteRenderer{})
                                .set(std::move(player));

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);

    assert(near(entity.get<kin::SpriteRenderer>()->rotation, 10.0f));
}

void test_unresolved_runtime_ref_placeholder_fails_silently() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::AnimationRegistry animations;
    animations.add(kin::Animation{.name = "bad.parent", .root = kin::ref_node("{X}.child")});

    kin::AnimationPlayer player = make_player(animations, properties, nullptr, "bad.parent");
    kin::EcsEntity entity = world.entity("bad-ref")
                                .set(kin::SpriteRenderer{.rotation = 7.0f})
                                .set(std::move(player));

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);

    const kin::AnimationPlayer* stored = entity.get<kin::AnimationPlayer>();
    assert(stored->layers.size() == 1);
    assert(stored->layers[0].animation->name == "bad.parent");
    assert(near(entity.get<kin::SpriteRenderer>()->rotation, 7.0f));
}

void test_override_mask_blends_allowed_numeric_property_and_pop() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::Clip base_clip{
        .duration = 1.0f,
        .properties = {
            vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {10.0f, 0.0f}, 1.0f),
            float_track("SpriteRenderer.rotation", 0.0f, 100.0f, 1.0f),
        },
    };
    kin::Clip override_clip{
        .duration = 0.25f,
        .properties = {
            vec_track("SpriteRenderer.offset", {100.0f, 0.0f}, {200.0f, 0.0f}, 0.25f),
            float_track("SpriteRenderer.rotation", 300.0f, 400.0f, 0.25f),
        },
    };

    kin::AnimationRegistry animations;
    animations.add(make_animation("base", std::move(base_clip), true));
    animations.add(make_animation("override", std::move(override_clip), false));

    kin::AnimationPlayer player = make_player(animations, properties, nullptr, "base");
    kin::push(player, "override", {.mask = {.properties = {"SpriteRenderer.offset"}}});
    kin::EcsEntity entity = world.entity("override")
                                .set(kin::SpriteRenderer{})
                                .set(std::move(player));

    kin::advance_animation_players(world, 0.125f);
    kin::sample_animation_players(world);
    const auto* renderer = entity.get<kin::SpriteRenderer>();
    assert(near(renderer->offset.x, 75.625f));
    assert(near(renderer->rotation, 12.5f));
    assert(entity.get<kin::AnimationPlayer>()->layers.size() == 2);

    kin::advance_animation_players(world, 0.125f);
    assert(entity.get<kin::AnimationPlayer>()->layers.size() == 1);
    kin::advance_animation_players(world, 0.125f);
    kin::sample_animation_players(world);
    renderer = entity.get<kin::SpriteRenderer>();
    assert(near(renderer->offset.x, 3.75f));
}

void test_numeric_layers_blend_by_normalized_weight() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::AnimationRegistry animations;
    animations.add(make_animation("base", kin::Clip{.duration = 1.0f, .properties = {vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {10.0f, 0.0f}, 1.0f)}}, true));
    animations.add(make_animation("override", kin::Clip{.duration = 1.0f, .properties = {vec_track("SpriteRenderer.offset", {100.0f, 0.0f}, {200.0f, 0.0f}, 1.0f)}}, true));

    kin::AnimationPlayer player = make_player(animations, properties, nullptr, "base");
    kin::push(player, "override", {.weight = 0.25f});
    kin::EcsEntity entity = world.entity("blend")
                                .set(kin::SpriteRenderer{})
                                .set(std::move(player));

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);

    const auto* renderer = entity.get<kin::SpriteRenderer>();
    assert(renderer != nullptr);
    assert(near(renderer->offset.x, 34.0f));
}

void test_zero_weight_layer_does_not_contribute() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::AnimationRegistry animations;
    animations.add(make_animation("base", kin::Clip{.duration = 1.0f, .properties = {float_track("SpriteRenderer.rotation", 0.0f, 10.0f, 1.0f)}}, true));
    animations.add(make_animation("override", kin::Clip{.duration = 1.0f, .properties = {float_track("SpriteRenderer.rotation", 100.0f, 200.0f, 1.0f)}}, true));

    kin::AnimationPlayer player = make_player(animations, properties, nullptr, "base");
    kin::push(player, "override", {.weight = 0.0f});
    kin::EcsEntity entity = world.entity("zero-weight")
                                .set(kin::SpriteRenderer{})
                                .set(std::move(player));

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);
    assert(near(entity.get<kin::SpriteRenderer>()->rotation, 5.0f));
}

void test_blend_in_ramps_effective_weight_and_snapshot() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::AnimationRegistry animations;
    animations.add(make_animation("base", kin::Clip{.duration = 1.0f, .properties = {float_track("SpriteRenderer.rotation", 0.0f, 0.0f, 1.0f)}}, true));
    animations.add(make_animation("override", kin::Clip{.duration = 1.0f, .properties = {float_track("SpriteRenderer.rotation", 100.0f, 100.0f, 1.0f)}}, true));

    kin::AnimationPlayer player = make_player(animations, properties, nullptr, "base");
    kin::push(player, "override", {.blend_in = 1.0f});
    kin::EcsEntity entity = world.entity("blend-in")
                                .set(kin::SpriteRenderer{})
                                .set(std::move(player));

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);

    const kin::AnimationPlayerSnapshot snap = kin::snapshot(*entity.get<kin::AnimationPlayer>());
    assert(snap.layers.size() == 2);
    assert(near(snap.layers[1].weight, 0.5f));
    assert(near(entity.get<kin::SpriteRenderer>()->rotation, 33.3333f));

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);
    const kin::AnimationPlayerSnapshot after = kin::snapshot(*entity.get<kin::AnimationPlayer>());
    assert(near(after.layers[1].weight, 1.0f));
    assert(near(entity.get<kin::SpriteRenderer>()->rotation, 50.0f));
}

void test_int_and_color_layers_blend_without_per_contribution_rounding() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);
    properties.add<TestAnimProperties>(world, "Test.count", &TestAnimProperties::count);

    kin::AnimationRegistry animations;
    animations.add(make_animation("base",
                                  kin::Clip{
                                      .duration = 1.0f,
                                      .properties = {
                                          value_track("Test.count", kin::i32{1}),
                                          value_track("SpriteRenderer.tint", kin::Color::rgba(1, 1, 1, 1)),
                                      },
                                  },
                                  true));
    animations.add(make_animation("override",
                                  kin::Clip{
                                      .duration = 1.0f,
                                      .properties = {
                                          value_track("Test.count", kin::i32{1}),
                                          value_track("SpriteRenderer.tint", kin::Color::rgba(1, 1, 1, 1)),
                                      },
                                  },
                                  true));

    kin::AnimationPlayer player = make_player(animations, properties, nullptr, "base");
    kin::push(player, "override");
    kin::EcsEntity entity = world.entity("int-color-blend")
                                .set(kin::SpriteRenderer{})
                                .set(TestAnimProperties{})
                                .set(std::move(player));

    kin::sample_animation_players(world);

    assert(entity.get<TestAnimProperties>()->count == 1);
    assert((entity.get<kin::SpriteRenderer>()->tint == kin::Color::rgba(1, 1, 1, 1)));
}

void test_bool_and_string_properties_remain_topmost_wins() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);
    properties.add<TestAnimProperties>(world, "Test.visible", &TestAnimProperties::visible);
    properties.add<TestAnimProperties>(world, "Test.label", &TestAnimProperties::label);

    kin::AnimationRegistry animations;
    animations.add(make_animation("base",
                                  kin::Clip{
                                      .duration = 1.0f,
                                      .properties = {
                                          value_track("Test.visible", false),
                                          value_track("Test.label", std::string{"base"}),
                                      },
                                  },
                                  true));
    animations.add(make_animation("override",
                                  kin::Clip{
                                      .duration = 1.0f,
                                      .properties = {
                                          value_track("Test.visible", true),
                                          value_track("Test.label", std::string{"override"}),
                                      },
                                  },
                                  true));

    kin::AnimationPlayer player = make_player(animations, properties, nullptr, "base");
    kin::push(player, "override", {.weight = 0.0f});
    kin::EcsEntity entity = world.entity("non-blendable-topmost")
                                .set(TestAnimProperties{})
                                .set(std::move(player));

    kin::sample_animation_players(world);

    const TestAnimProperties* props = entity.get<TestAnimProperties>();
    assert(props->visible);
    assert(props->label == "override");
}

void test_sprite_override_wins_then_base_resumes() {
    kin::EcsWorld world;
    register_components(world);
    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);
    kin::SpriteCatalog catalog = make_catalog();

    kin::SpriteTrack base_sprites;
    base_sprites.keys = {{.time = 0.0f, .sprite_id = "hero.0"}};
    kin::SpriteTrack override_sprites;
    override_sprites.keys = {{.time = 0.0f, .sprite_id = "hero.1"}};

    kin::AnimationRegistry animations;
    animations.add(make_animation("sprite.base", kin::Clip{.duration = 1.0f, .sprites = {std::move(base_sprites)}}, true));
    animations.add(make_animation("sprite.override", kin::Clip{.duration = 0.2f, .sprites = {std::move(override_sprites)}}, false));

    kin::AnimationPlayer player = make_player(animations, properties, &catalog, "sprite.base");
    kin::push(player, "sprite.override");
    kin::EcsEntity entity = world.entity("sprite-override")
                                .set(kin::SpriteRenderer{})
                                .set(std::move(player));

    kin::sample_animation_players(world);
    assert(entity.get<kin::SpriteRenderer>()->sprite.id == "hero.1");

    kin::advance_animation_players(world, 0.2f);
    kin::sample_animation_players(world);
    assert(entity.get<kin::AnimationPlayer>()->layers.size() == 1);
    assert(entity.get<kin::SpriteRenderer>()->sprite.id == "hero.0");
}

void test_target_property_track_writes_child_not_root() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::PropertyTrack child_offset = vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {8.0f, 4.0f}, 1.0f);
    child_offset.target = "arm";

    kin::AnimationRegistry animations;
    animations.add(make_animation("child.offset", kin::Clip{.duration = 1.0f, .properties = {std::move(child_offset)}}, false));

    kin::EcsEntity root = world.entity("root")
                              .set(kin::SpriteRenderer{.offset = {100.0f, 100.0f}})
                              .set(make_player(animations, properties, nullptr, "child.offset"));
    kin::EcsEntity arm = world.entity("arm").set(kin::SpriteRenderer{}).child_of(root);

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);

    assert((root.get<kin::SpriteRenderer>()->offset == kin::Vec2f{100.0f, 100.0f}));
    assert((arm.get<kin::SpriteRenderer>()->offset == kin::Vec2f{4.0f, 2.0f}));
}

void test_nested_target_and_relative_child_base() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::PropertyTrack relative = vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {4.0f, 6.0f}, 1.0f);
    relative.target = "body/arm";
    relative.space = kin::TrackSpace::Relative;

    kin::AnimationRegistry animations;
    animations.add(make_animation("nested.relative", kin::Clip{.duration = 1.0f, .properties = {std::move(relative)}}, false));

    kin::EcsEntity root = world.entity("root-nested")
                              .set(kin::SpriteRenderer{.offset = {50.0f, 50.0f}})
                              .set(make_player(animations, properties, nullptr, "nested.relative"));
    kin::EcsEntity body = world.entity("body").child_of(root);
    kin::EcsEntity arm = world.entity("arm").set(kin::SpriteRenderer{.offset = {10.0f, 20.0f}}).child_of(body);

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);
    assert((arm.get<kin::SpriteRenderer>()->offset == kin::Vec2f{12.0f, 23.0f}));

    arm.get_mut<kin::SpriteRenderer>()->offset = {100.0f, 100.0f};
    kin::advance_animation_players(world, 0.25f);
    kin::sample_animation_players(world);
    assert((arm.get<kin::SpriteRenderer>()->offset == kin::Vec2f{13.0f, 24.5f}));
}

void test_target_sprite_track_writes_child_not_root() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);
    kin::SpriteCatalog catalog = make_catalog();

    kin::SpriteTrack sprites;
    sprites.target = "arm";
    sprites.keys = {{.time = 0.0f, .sprite_id = "hero.1", .pivot = {0.2f, 0.8f}, .has_pivot = true}};

    kin::AnimationRegistry animations;
    animations.add(make_animation("child.sprite", kin::Clip{.duration = 0.2f, .sprites = {std::move(sprites)}}, false));

    kin::EcsEntity root = world.entity("root-sprite")
                              .set(kin::SpriteRenderer{.sprite = catalog.ref("hero.0")})
                              .set(make_player(animations, properties, &catalog, "child.sprite"));
    kin::EcsEntity arm = world.entity("arm").set(kin::SpriteRenderer{}).child_of(root);

    kin::sample_animation_players(world);

    assert(root.get<kin::SpriteRenderer>()->sprite.id == "hero.0");
    assert(arm.get<kin::SpriteRenderer>()->sprite.id == "hero.1");
    assert((arm.get<kin::SpriteRenderer>()->pivot == kin::Vec2f{0.2f, 0.8f}));
}

void test_target_property_samples_do_not_clobber_each_other() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::PropertyTrack left = vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {10.0f, 0.0f}, 1.0f);
    left.target = "left";
    kin::PropertyTrack right = vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {0.0f, 20.0f}, 1.0f);
    right.target = "right";

    kin::AnimationRegistry animations;
    animations.add(make_animation("two.children", kin::Clip{.duration = 1.0f, .properties = {std::move(left), std::move(right)}}, false));

    kin::EcsEntity root = world.entity("root-two").set(make_player(animations, properties, nullptr, "two.children"));
    kin::EcsEntity left_entity = world.entity("left").set(kin::SpriteRenderer{}).child_of(root);
    kin::EcsEntity right_entity = world.entity("right").set(kin::SpriteRenderer{}).child_of(root);

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);

    assert((left_entity.get<kin::SpriteRenderer>()->offset == kin::Vec2f{5.0f, 0.0f}));
    assert((right_entity.get<kin::SpriteRenderer>()->offset == kin::Vec2f{0.0f, 10.0f}));
}

void test_missing_target_track_is_silent() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::PropertyTrack missing = vec_track("SpriteRenderer.offset", {0.0f, 0.0f}, {10.0f, 10.0f}, 1.0f);
    missing.target = "missing";

    kin::AnimationRegistry animations;
    animations.add(make_animation("missing.target", kin::Clip{.duration = 1.0f, .properties = {std::move(missing)}}, false));

    kin::EcsEntity root = world.entity("root-missing")
                              .set(kin::SpriteRenderer{.offset = {7.0f, 8.0f}})
                              .set(make_player(animations, properties, nullptr, "missing.target"));

    kin::advance_animation_players(world, 0.5f);
    kin::sample_animation_players(world);

    assert((root.get<kin::SpriteRenderer>()->offset == kin::Vec2f{7.0f, 8.0f}));
}

void test_player_config_and_snapshot_expose_runtime_state() {
    kin::EcsWorld world;
    register_components(world);

    kin::PropertyRegistry properties;
    kin::register_builtin_properties(properties, world);

    kin::AnimationRegistry animations;
    animations.add(make_animation("idle", kin::Clip{.duration = 1.0f, .properties = {float_track("SpriteRenderer.rotation", 0.0f, 10.0f, 1.0f)}}, false));
    animations.add(make_animation("blink", kin::Clip{.duration = 0.5f, .properties = {float_track("SpriteRenderer.rotation", 20.0f, 30.0f, 0.5f)}}, false));

    kin::AnimationPlayer player = kin::make_animation_player({
        .registry = &animations,
        .properties = &properties,
        .bindings = {{"X", "hero"}},
        .base_animation = "idle",
    });
    player.state = "idle";
    player.params.set_trigger("hit");
    kin::push(player, "blink", {.speed = 2.0f});

    kin::EcsEntity entity = world.entity("snapshot").set(kin::SpriteRenderer{}).set(std::move(player));
    kin::advance_animation_players(world, 0.1f);

    const kin::AnimationPlayerSnapshot snap = kin::snapshot(*entity.get<kin::AnimationPlayer>());
    assert(snap.state == "idle");
    assert(snap.playing);
    assert(snap.bindings.at("X") == "hero");
    assert(snap.triggers.size() == 1);
    assert(snap.triggers[0] == "hit");
    assert(snap.layers.size() == 2);
    assert(snap.layers[0].kind == kin::LayerKind::Base);
    assert(snap.layers[0].animation == "idle");
    assert(near(snap.layers[0].time, 0.1f));
    assert(snap.layers[1].kind == kin::LayerKind::Override);
    assert(snap.layers[1].animation == "blink");
    assert(near(snap.layers[1].time, 0.2f));
    assert(near(snap.layers[1].speed, 2.0f));
}

void test_animation_preview_base_override_sampling_and_missing_preserves_state() {
    kin::AnimationRegistry animations;

    kin::Clip base_clip{
        .duration = 1.0f,
        .properties = {float_track("SpriteRenderer.rotation", 0.0f, 10.0f, 1.0f)},
    };
    kin::SpriteTrack sprites;
    sprites.keys = {{.time = 0.0f, .sprite_id = "hero.0"}};
    base_clip.sprites.push_back(std::move(sprites));
    animations.add(make_animation("base", std::move(base_clip), false));

    animations.add(make_animation("override",
                                  kin::Clip{.duration = 0.2f, .properties = {float_track("SpriteRenderer.rotation", 100.0f, 200.0f, 0.2f)}},
                                  false));

    kin::SpriteCatalog catalog = make_catalog();
    kin::AnimationPreview preview{{.registry = &animations, .sprite_catalog = &catalog}};

    assert(preview.set_animation("base"));
    preview.advance(0.5f);
    preview.sample();
    assert(near(preview.entity().get<kin::SpriteRenderer>()->rotation, 5.0f));
    assert(preview.entity().get<kin::SpriteRenderer>()->sprite.id == "hero.0");

    const kin::AnimationPlayerSnapshot before_missing = preview.snapshot();
    assert(!preview.set_animation("missing"));
    assert(preview.snapshot().layers.size() == before_missing.layers.size());
    assert(preview.snapshot().layers[0].animation == "base");
    assert(near(preview.snapshot().layers[0].time, before_missing.layers[0].time));

    assert(preview.push("override"));
    assert(preview.snapshot().layers.size() == 2);
    preview.advance(0.2f);
    assert(preview.snapshot().layers.size() == 1);
    assert(preview.snapshot().layers[0].animation == "base");
}

} // namespace

int main() {
    test_builtin_property_binding();
    test_looping_property_track_drives_sprite_offset();
    test_relative_track_captures_per_entity_start_value_once();
    test_sprite_track_sets_sprite_and_key_pivot();
    test_sprite_track_preserves_existing_catalog_when_player_has_none();
    test_sequence_advances_with_dt_carryover();
    test_parallel_end_policies();
    test_finite_repeat_finishes_and_infinite_repeat_keeps_looping();
    test_ref_resolves_and_cycle_does_not_hang();
    test_runtime_ref_names_resolve_through_player_bindings();
    test_unresolved_runtime_ref_placeholder_fails_silently();
    test_override_mask_blends_allowed_numeric_property_and_pop();
    test_numeric_layers_blend_by_normalized_weight();
    test_zero_weight_layer_does_not_contribute();
    test_blend_in_ramps_effective_weight_and_snapshot();
    test_int_and_color_layers_blend_without_per_contribution_rounding();
    test_bool_and_string_properties_remain_topmost_wins();
    test_sprite_override_wins_then_base_resumes();
    test_target_property_track_writes_child_not_root();
    test_nested_target_and_relative_child_base();
    test_target_sprite_track_writes_child_not_root();
    test_target_property_samples_do_not_clobber_each_other();
    test_missing_target_track_is_silent();
    test_player_config_and_snapshot_expose_runtime_state();
    test_animation_preview_base_override_sampling_and_missing_preserves_state();
    return 0;
}
