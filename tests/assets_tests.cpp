#include <kin/assets/asset_manager.hpp>
#include <kin/assets/asset_server.hpp>
#include <kin/assets/image.hpp>
#include <kin/dialogue/dialogue.hpp>
#include <kin/anim/anim_format.hpp>
#include <kin/audio/audio_catalog.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/platform/app.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/renderer/animation_import.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite_catalog.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/ui2/skin_loader.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <variant>
#include <vector>

namespace {

struct AsyncProbeAsset {
    int value = 0;
};

bool has_log_event(const std::vector<kin::LogEvent>& events,
                   std::string_view category,
                   std::string_view message) {
    return std::ranges::any_of(events, [&](const kin::LogEvent& event) {
        return event.category == category && event.message == message;
    });
}

void write_bytes(const std::filesystem::path& path, const std::vector<kin::u8>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_bmp(const std::filesystem::path& path) {
    const std::vector<kin::u8> bytes{
        0x42, 0x4d, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xfe, 0xff,
        0xff, 0xff, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // Top-down 2x2 BGRA pixels.
        0x00, 0x00, 0xff, 0xff,
        0x00, 0xff, 0x00, 0xff,
        0xff, 0x00, 0x00, 0xff,
        0xff, 0xff, 0xff, 0xff,
    };
    write_bytes(path, bytes);
}

void write_png(const std::filesystem::path& path) {
    // A real, valid 1x1 RGBA PNG written via the engine encoder. (The previous
    // hand-crafted byte blob had a bad IDAT CRC that libpng tolerated pre-SDL_image-3.4
    // but newer libpng rejects as fatal.)
    kin::Image image;
    image.size = {1, 1};
    image.rgba = {0xff, 0x00, 0x00, 0xff}; // one opaque red pixel
    const bool ok = kin::save_image(image, path);
    assert(ok);
    (void)ok;
}

void check_image(const kin::Image& image, kin::Vec2i expected_size) {
    assert(image.valid());
    assert(image.size == expected_size);
    assert(image.rgba.size() == static_cast<std::size_t>(image.size.x * image.size.y * 4));
}

} // namespace

void run_anim_asset_loader_tests();

int main() {
    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-assets-tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const auto bmp_path = dir / "tiny.bmp";
    const auto png_path = dir / "tiny.png";
    const auto txt_path = dir / "notes.txt";
    const auto info_path = dir / "game.kininfo";
    const auto input_path = dir / "controls.kininput";
    const auto anim_path = dir / "hero.kinanim";
    const auto state_machine_path = dir / "hero.kinanimsm";
    const auto catalog_path = dir / "hero.kinsprites";
    const auto audio_path = dir / "audio.kinaudio";
    const auto prefab_path = dir / "panel.kinprefab";
    const auto script_path = dir / "scene.lua";
    write_bmp(bmp_path);
    write_png(png_path);
    std::ofstream{txt_path} << "hello";
    std::ofstream{script_path} << "function update(ctx) end\n";
    kin::GameInfo saved_info{
        .id = "asset-test",
        .title = "Asset Test",
        .version = "1.0",
        .window = {
            .width = 320,
            .height = 180,
            .logical_width = 160,
            .logical_height = 90,
            .integer_scale = true,
            .resizable = true,
        },
        .tags = {"asset", "metadata"},
    };
    kin::set_field(saved_info, "genre", "test");
    assert(kin::save_game_info(saved_info, info_path));
    kin::InputMap saved_input;
    saved_input.bind("confirm", kin::Key::Enter);
    saved_input.bind("confirm", kin::MouseButton::Left);
    saved_input.bind("copy", kin::Key::C, kin::KeyModifiers::Ctrl);
    assert(kin::save_input_map(saved_input, input_path));
    std::ofstream{anim_path}
        << "animation hero.attack\n"
        << "  clip 0.12\n"
        << "    sprite hero.attack.0 0\n"
        << "    sprite hero.attack.1 0.06 pivot 0.25 0.75\n"
        << "    event 0.06 sound name=sfx value=slash offset 0 -8\n";
    std::ofstream{state_machine_path} << "# legacy state machines are not loaded here\n";
    kin::SpriteCatalog saved_catalog;
    saved_catalog.set_texture("tiny", "tiny.bmp", {});
    saved_catalog.add_sheet({.id = "tiny.sheet", .texture_id = "tiny", .grid = {.tile_w = 1, .tile_h = 1}});
    saved_catalog.add_sheet_sprite("hero.sheet.1", "tiny.sheet", 1, {8.0f, 8.0f});
    saved_catalog.add({
        .id = "hero.direct",
        .texture_id = "tiny",
        .source = {0.0f, 0.0f, 1.0f, 1.0f},
        .size = {8.0f, 8.0f},
        .pivot = {0.5f, 1.0f},
    });
    assert(kin::save_sprite_catalog(saved_catalog, catalog_path));
    kin::AudioCatalog saved_audio;
    saved_audio.set_root(dir);
    saved_audio.add_bus({.id = "master", .volume = 1.0f});
    saved_audio.add_bus({.id = "sfx", .parent = "master", .volume = 0.8f});
    saved_audio.add_clip({.id = "step", .path = "step.wav"});
    saved_audio.add_cue({
        .id = "footstep",
        .clips = {"step"},
        .category = kin::AudioCategory::Sound,
        .bus = "sfx",
        .priority = 10,
    });
    assert(kin::save_audio_catalog(saved_audio, audio_path));
    std::ofstream{prefab_path} << R"json({
  "schema": "kin.prefab/1",
  "id": "panel",
  "root": "root",
  "entities": [
    {
      "id": "root",
      "components": {
        "Transform2D": { "pos": { "x": 1, "y": 2 }, "rotation": 0 }
      }
    }
  ]
}
)json";

    const kin::Image bmp = kin::load_image(bmp_path);
    const kin::Vec2i bmp_size{2, 2};
    check_image(bmp, bmp_size);

    const kin::Image png = kin::load_image(png_path);
    const kin::Vec2i png_size{1, 1};
    check_image(png, png_size);

    kin::AssetManager assets{dir};
    assert(assets.stats().registered_loaders >= 1);
    assert(assets.resolve("tiny.bmp") == bmp_path);
    assert(kin::asset_type_name(kin::AssetType::Image) == "Image");
    assert(kin::asset_status_name(kin::AssetStatus::Loaded) == "Loaded");
    assert(has_log_event(log_events, "asset", "asset manager created"));

    assets.discover();
    assert(has_log_event(log_events, "asset", "asset discovery complete"));
    const auto discovered = assets.assets_metadata();
    assert(discovered.size() == 11);
    const kin::AssetMetadata* bmp_meta = assets.metadata("tiny.bmp");
    assert(bmp_meta != nullptr);
    assert(bmp_meta->name == "tiny.bmp");
    assert(bmp_meta->path == "tiny.bmp");
    assert(bmp_meta->type == kin::AssetType::Image);
    assert(bmp_meta->status == kin::AssetStatus::Discovered);
    assert(bmp_meta->size_bytes > 0);
    const kin::AssetMetadata* txt_meta = assets.metadata("notes.txt");
    assert(txt_meta != nullptr);
    assert(txt_meta->type == kin::AssetType::Unknown);
    const kin::AssetMetadata* anim_meta = assets.metadata("hero.kinanim");
    assert(anim_meta != nullptr);
    assert(anim_meta->type == kin::AssetType::Animation);
    assert(anim_meta->status == kin::AssetStatus::Discovered);
    const kin::AssetMetadata* state_machine_meta = assets.metadata("hero.kinanimsm");
    assert(state_machine_meta != nullptr);
    assert(state_machine_meta->type == kin::AssetType::AnimationStateMachine);
    assert(state_machine_meta->status == kin::AssetStatus::Discovered);
    const kin::AssetMetadata* info_meta = assets.metadata("game.kininfo");
    assert(info_meta != nullptr);
    assert(info_meta->type == kin::AssetType::GameInfo);
    assert(info_meta->status == kin::AssetStatus::Discovered);
    const kin::AssetMetadata* input_meta = assets.metadata("controls.kininput");
    assert(input_meta != nullptr);
    assert(input_meta->type == kin::AssetType::InputMap);
    assert(input_meta->status == kin::AssetStatus::Discovered);
    const kin::AssetMetadata* catalog_meta = assets.metadata("hero.kinsprites");
    assert(catalog_meta != nullptr);
    assert(catalog_meta->type == kin::AssetType::SpriteCatalog);
    assert(catalog_meta->status == kin::AssetStatus::Discovered);
    const kin::AssetMetadata* audio_meta = assets.metadata("audio.kinaudio");
    assert(audio_meta != nullptr);
    assert(audio_meta->type == kin::AssetType::AudioCatalog);
    assert(audio_meta->status == kin::AssetStatus::Discovered);
    const kin::AssetMetadata* script_meta = assets.metadata("scene.lua");
    assert(script_meta != nullptr);
    assert(script_meta->type == kin::AssetType::Script);
    assert(script_meta->status == kin::AssetStatus::Discovered);
    const kin::AssetMetadata* prefab_meta = assets.metadata("panel.kinprefab");
    assert(prefab_meta != nullptr);
    assert(prefab_meta->type == kin::AssetType::Prefab);
    assert(prefab_meta->status == kin::AssetStatus::Discovered);

    const auto loaded_bmp = assets.load<kin::Image>("tiny.bmp");
    assert(loaded_bmp);
    check_image(*loaded_bmp, bmp_size);
    assert(assets.loaded<kin::Image>("tiny.bmp"));
    assert(assets.stats().loaded_assets == 1);
    kin::AssetHandle<kin::Image> bmp_handle = assets.load_handle<kin::Image>("tiny.bmp");
    assert(bmp_handle);
    assert(bmp_handle.shared() == loaded_bmp);
    assert(bmp_handle->size == bmp_size);
    kin::Image replacement = *loaded_bmp;
    replacement.size = {3, 1};
    replacement.rgba.resize(12);
    assert(assets.replace_loaded<kin::Image>("tiny.bmp", replacement));
    assert((bmp_handle->size == kin::Vec2i{3, 1}));
    assert((loaded_bmp->size == kin::Vec2i{3, 1}));
    assert(assets.replace_loaded<kin::Image>("missing.bmp", replacement) == false);
    assert(assets.replace_loaded<kin::Image>("tiny.bmp", bmp));
    assert(bmp_handle->size == bmp_size);
    bmp_meta = assets.metadata("tiny.bmp");
    assert(bmp_meta != nullptr);
    assert(bmp_meta->status == kin::AssetStatus::Loaded);
    assert(bmp_meta->dimensions == bmp_size);

    const auto cached_bmp = assets.load<kin::Image>("tiny.bmp");
    assert(cached_bmp == loaded_bmp);
    assert(assets.stats().loaded_assets == 1);
    assert(has_log_event(log_events, "asset", "asset cache hit"));

    const auto loaded_png = assets.load<kin::Image>("tiny.png");
    assert(loaded_png);
    check_image(*loaded_png, png_size);
    assert(has_log_event(log_events, "asset", "asset loaded"));
    assert(has_log_event(log_events, "asset", "image loaded"));
    assert(assets.stats().loaded_assets == 2);
    assert(assets.loaded_asset_paths().size() == 2);
    const kin::AssetMetadata* png_meta = assets.metadata("tiny.png");
    assert(png_meta != nullptr);
    assert(png_meta->status == kin::AssetStatus::Loaded);
    assert(png_meta->dimensions == png_size);

    const auto loaded_animation = assets.load<kin::AnimationRegistryFragment>("hero.kinanim");
    assert(loaded_animation);
    assert(loaded_animation->animations.size() == 1);
    const kin::Animation& attack_animation = loaded_animation->animations[0];
    assert(attack_animation.name == "hero.attack");
    const auto* attack = std::get_if<kin::Clip>(&attack_animation.root.value);
    assert(attack != nullptr);
    assert(attack->duration == 0.12f);
    assert(attack->sprites.size() == 1);
    assert(attack->sprites[0].keys.size() == 2);
    assert(attack->sprites[0].keys[1].sprite_id == "hero.attack.1");
    assert(attack->sprites[0].keys[1].has_pivot);
    assert(attack->events.size() == 1);
    assert(attack->events[0].keys.size() == 1);
    assert(attack->events[0].keys[0].event.channel == "sound");
    assert(attack->events[0].keys[0].event.value == "slash");
    anim_meta = assets.metadata("hero.kinanim");
    assert(anim_meta != nullptr);
    assert(anim_meta->type == kin::AssetType::Animation);
    assert(anim_meta->status == kin::AssetStatus::Loaded);


    const auto loaded_info = assets.load<kin::GameInfo>("game.kininfo");
    assert(loaded_info);
    assert(loaded_info->id == "asset-test");
    assert(loaded_info->title == "Asset Test");
    assert(loaded_info->window.integer_scale);
    assert(loaded_info->tags.size() == 2);
    assert(kin::field(*loaded_info, "genre") != nullptr);
    assert(*kin::field(*loaded_info, "genre") == "test");
    info_meta = assets.metadata("game.kininfo");
    assert(info_meta != nullptr);
    assert(info_meta->type == kin::AssetType::GameInfo);
    assert(info_meta->status == kin::AssetStatus::Loaded);

    const auto loaded_input = assets.load<kin::InputMap>("controls.kininput");
    assert(loaded_input);
    const auto input_actions = loaded_input->actions();
    assert(input_actions.size() == 2);
    assert(input_actions[0].name == "confirm");
    assert(input_actions[0].bindings.size() == 2);
    assert(kin::binding_name(input_actions[0].bindings[0]) == "Enter");
    assert(kin::binding_name(input_actions[0].bindings[1]) == "MouseLeft");
    assert(input_actions[1].name == "copy");
    assert(input_actions[1].bindings[0].modifiers == kin::KeyModifiers::Ctrl);
    input_meta = assets.metadata("controls.kininput");
    assert(input_meta != nullptr);
    assert(input_meta->type == kin::AssetType::InputMap);
    assert(input_meta->status == kin::AssetStatus::Loaded);

    const auto loaded_audio = assets.load<kin::AudioCatalog>("audio.kinaudio");
    assert(loaded_audio);
    assert(loaded_audio->cue("footstep") != nullptr);
    assert(loaded_audio->clip("step") != nullptr);
    audio_meta = assets.metadata("audio.kinaudio");
    assert(audio_meta != nullptr);
    assert(audio_meta->type == kin::AssetType::AudioCatalog);
    assert(audio_meta->status == kin::AssetStatus::Loaded);

    const auto loaded_prefab = assets.load<kin::PrefabAsset>("panel.kinprefab");
    assert(loaded_prefab);
    assert(loaded_prefab->id == "panel");
    prefab_meta = assets.metadata("panel.kinprefab");
    assert(prefab_meta != nullptr);
    assert(prefab_meta->type == kin::AssetType::Prefab);
    assert(prefab_meta->status == kin::AssetStatus::Loaded);

    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({
        .title = "assets-test",
        .width = 32,
        .height = 32,
        .hidden = true,
    });
    kin::Renderer2D renderer{window};
    const kin::Texture texture = renderer.create_texture_from_rgba(loaded_bmp->rgba.data(), loaded_bmp->size);
    assert(texture);
    assert(texture.size() == bmp_size);

    kin::SpriteCatalog loaded_catalog = kin::load_sprite_catalog(assets, renderer, catalog_path);
    kin::ResolvedSprite resolved;
    assert(loaded_catalog.resolve("hero.direct", resolved));
    assert(resolved.sprite.valid());
    assert((resolved.size == kin::Vec2f{8.0f, 8.0f}));
    assert(loaded_catalog.resolve("hero.sheet.1", resolved));
    assert(resolved.sprite.source.x == 1.0f);

    kin::register_sprite_catalog_loader(assets, renderer);
    kin::AssetHandle<kin::SpriteCatalog> loaded_catalog_asset = assets.load_handle<kin::SpriteCatalog>("hero.kinsprites");
    assert(loaded_catalog_asset);
    assert(loaded_catalog_asset->contains("hero.direct"));
    catalog_meta = assets.metadata("hero.kinsprites");
    assert(catalog_meta != nullptr);
    assert(catalog_meta->status == kin::AssetStatus::Loaded);

    kin::AnimationImportSet import{
        .texture_id = "tiny",
        .texture_path = "tiny.bmp",
        .clips = {{
            .name = "hero.walk",
            .frames = {
                {.source = {0.0f, 0.0f, 1.0f, 1.0f}, .duration = 0.1f},
                {
                    .source = {1.0f, 0.0f, 1.0f, 1.0f},
                    .duration = 0.2f,
                    .pivot = {0.25f, 0.75f},
                    .has_pivot = true,
                    .boxes = {{
                        .kind = kin::SpriteBox::Kind::Hitbox,
                        .rect = {8.0f, -12.0f, 14.0f, 8.0f},
                        .name = "sword",
                    }},
                },
            },
            .playback = kin::AnimationImportPlayback::Once,
        }},
    };
    kin::SpriteCatalog imported_catalog;
    kin::AnimationRegistry imported_animation;
    assert(kin::add_animation_sprites(imported_catalog,
                                      assets,
                                      renderer,
                                      import,
                                      {.id_prefix = "player", .draw_size = {16.0f, 16.0f}},
                                      &imported_animation));
    assert(imported_catalog.contains("player.hero.walk.0"));
    assert(imported_catalog.resolve("player.hero.walk.1", resolved));
    assert(resolved.sprite.source.x == 1.0f);
    assert((resolved.size == kin::Vec2f{16.0f, 16.0f}));
    const auto imported_clip_animation = imported_animation.resolve("hero.walk");
    assert(imported_clip_animation);
    const auto* imported_clip = std::get_if<kin::Clip>(&imported_clip_animation->root.value);
    assert(imported_clip != nullptr);
    assert(imported_clip->duration == 0.3f);
    assert(imported_clip->sprites.size() == 1);
    assert(imported_clip->sprites[0].keys[1].sprite_id == "player.hero.walk.1");
    assert(imported_clip->sprites[0].keys[1].has_pivot);
    assert(imported_clip->sprites[0].keys[1].boxes.size() == 1);
    assert(imported_clip->sprites[0].keys[1].boxes[0].kind == kin::SpriteBox::Kind::Hitbox);

    kin::AnimationImportSet playback_import{
        .texture_id = "tiny",
        .texture_path = "tiny.bmp",
        .clips = {
            {
                .name = "loop",
                .frames = {
                    {.source = {0.0f, 0.0f, 1.0f, 1.0f}, .duration = 0.1f},
                    {
                        .source = {1.0f, 0.0f, 1.0f, 1.0f},
                        .duration = 0.2f,
                        .events = {{.channel = "sound", .name = "step", .value = "step"}},
                    },
                },
                .playback = kin::AnimationImportPlayback::Loop,
            },
            {
                .name = "ping",
                .frames = {
                    {.source = {0.0f, 0.0f, 1.0f, 1.0f}, .duration = 0.1f},
                    {.source = {1.0f, 0.0f, 1.0f, 1.0f}, .duration = 0.2f},
                    {.source = {0.0f, 1.0f, 1.0f, 1.0f}, .duration = 0.3f},
                },
                .playback = kin::AnimationImportPlayback::PingPong,
            },
        },
    };
    kin::AnimationRegistry playback_animations;
    assert(kin::add_animation_sprites(imported_catalog,
                                      playback_import,
                                      {.id_prefix = "player"},
                                      &playback_animations));
    const auto loop_animation = playback_animations.resolve("loop");
    assert(loop_animation);
    const auto* loop_repeat = std::get_if<kin::Repeat>(&loop_animation->root.value);
    assert(loop_repeat != nullptr);
    assert(loop_repeat->count == 0);
    assert(loop_repeat->child != nullptr);
    const auto* loop_clip = std::get_if<kin::Clip>(&loop_repeat->child->value);
    assert(loop_clip != nullptr);
    assert(loop_clip->events.size() == 1);
    assert(loop_clip->events[0].keys[0].time == 0.1f);
    assert(loop_clip->events[0].keys[0].event.value == "step");

    const auto ping_animation = playback_animations.resolve("ping");
    assert(ping_animation);
    const auto* ping_repeat = std::get_if<kin::Repeat>(&ping_animation->root.value);
    assert(ping_repeat != nullptr);
    assert(ping_repeat->count == 0);
    const auto* ping_sequence = std::get_if<kin::Sequence>(&ping_repeat->child->value);
    assert(ping_sequence != nullptr);
    assert(ping_sequence->children.size() == 2);
    const auto* ping_forward = std::get_if<kin::Clip>(&ping_sequence->children[0].value);
    const auto* ping_reverse = std::get_if<kin::Clip>(&ping_sequence->children[1].value);
    assert(ping_forward != nullptr);
    assert(ping_reverse != nullptr);
    assert(ping_forward->sprites[0].keys.size() == 3);
    assert(ping_reverse->sprites[0].keys.size() == 2);
    assert(ping_reverse->sprites[0].keys[0].sprite_id == "player.ping.1");
    assert(ping_reverse->sprites[0].keys[1].sprite_id == "player.ping.0");

    assets.unload("tiny.bmp");
    assert(!assets.loaded<kin::Image>("tiny.bmp"));
    assert(!bmp_handle);
    assert(assets.stats().loaded_assets == 7);
    bmp_meta = assets.metadata("tiny.bmp");
    assert(bmp_meta != nullptr);
    assert(bmp_meta->status == kin::AssetStatus::Discovered);

    assets.clear();
    assert(assets.stats().loaded_assets == 0);
    assert(!loaded_catalog_asset);
    png_meta = assets.metadata("tiny.png");
    assert(png_meta != nullptr);
    assert(png_meta->status == kin::AssetStatus::Discovered);

    // Skin loader bridge: save_image -> AssetManager load<Image> -> Texture -> UiNineSlice.
    {
        kin::Image skin_img;
        skin_img.size = {32, 32};
        skin_img.rgba.assign(static_cast<std::size_t>(32 * 32 * 4), kin::u8{200});
        const auto skin_png = dir / "skins" / "frame.png";
        assert(kin::save_image(skin_img, skin_png));
        assert(std::filesystem::exists(skin_png));

        kin::AssetManager skin_assets{dir};
        kin::register_default_asset_loaders(skin_assets);

        const kin::ui2::UiNineSlice slice =
            kin::ui2::load_nine_slice(skin_assets, renderer, "skins/frame.png", 8.0f);
        assert(slice.sprite.valid());
        assert(slice.left == 8.0f && slice.top == 8.0f && slice.right == 8.0f && slice.bottom == 8.0f);
        assert(slice.sprite.texture.size() == (kin::Vec2i{32, 32}));

        // nine_slice over a sub-rect of an already-uploaded texture (atlas slicing, one upload).
        kin::Texture atlas = kin::ui2::load_texture(skin_assets, renderer, "skins/frame.png");
        const kin::ui2::UiNineSlice sub = kin::ui2::nine_slice(atlas, 4.0f, {0.0f, 0.0f, 16.0f, 16.0f});
        assert(sub.sprite.valid());
        assert(sub.sprite.source.w == 16.0f && sub.sprite.source.h == 16.0f);

        // The Image is cached by the AssetManager (one decode for repeated loads).
        assert(skin_assets.loaded<kin::Image>("skins/frame.png"));
    }

    // ---- Async AssetServer: handle-first loading, events, determinism ----
    {
        // Pending immediately; completes after a deterministic drain.
        kin::AssetManager srv_assets{dir};
        kin::AssetServer server{srv_assets, {.worker_count = 4}};

        kin::AssetHandle<kin::Image> async_png = server.load_async<kin::Image>("tiny.png");
        assert(!async_png);
        assert(server.load_state<kin::Image>("tiny.png") == kin::LoadState::Loading);

        // Unloading an in-flight request must invalidate the server state and
        // discard its eventual completion rather than emitting a stale Added.
        kin::AssetManager probe_assets{dir};
        kin::AssetServer probe_server{probe_assets, {.worker_count = 1}};
        probe_server.register_async_loader<AsyncProbeAsset>([](const auto&, auto&) {
            return AsyncProbeAsset{42};
        });
        auto pending_probe = probe_server.load_async<AsyncProbeAsset>("pending.probe");
        probe_assets.unload("pending.probe");
        assert(probe_server.load_state<AsyncProbeAsset>("pending.probe") == kin::LoadState::NotLoaded);
        probe_server.drain();
        assert(!pending_probe);
        bool stale_probe_event = false;
        for (const kin::AssetEvent& event : probe_server.events()) {
            stale_probe_event = stale_probe_event || event.path == "pending.probe";
        }
        assert(!stale_probe_event);

        server.drain();
        assert(async_png);
        assert(async_png->size == png_size);
        assert(server.load_state<kin::Image>("tiny.png") == kin::LoadState::Loaded);
        assert(server.ready<kin::Image>("tiny.png"));

        bool png_added = false;
        for (const kin::AssetEvent& event : server.events()) {
            if (event.kind == kin::AssetEvent::Kind::Added && event.path == "tiny.png") {
                png_added = true;
            }
        }
        assert(png_added);

        // A repeated request is deduped: no new event on the next drain.
        server.load_async<kin::Image>("tiny.png");
        server.drain();
        assert(server.events().empty());

        // AssetManager unload/clear may invalidate storage behind the server;
        // the next async request must schedule a fresh load rather than trusting
        // the server's old Loaded state.
        srv_assets.unload("tiny.png");
        assert(!srv_assets.loaded<kin::Image>("tiny.png"));
        kin::AssetHandle<kin::Image> reloaded_png = server.load_async<kin::Image>("tiny.png");
        assert(!reloaded_png);
        assert(server.load_state<kin::Image>("tiny.png") == kin::LoadState::Loading);
        server.drain();
        assert(reloaded_png);
        assert(server.ready<kin::Image>("tiny.png"));

        // A failing loader yields LoadState::Failed + a Failed event (no throw).
        kin::AssetHandle<kin::DialogueDocument> bad =
            server.load_async<kin::DialogueDocument>("does-not-exist.kindialogue");
        server.drain();
        assert(!bad);
        assert(server.load_state<kin::DialogueDocument>("does-not-exist.kindialogue") ==
               kin::LoadState::Failed);
        bool dialogue_failed = false;
        for (const kin::AssetEvent& event : server.events()) {
            if (event.kind == kin::AssetEvent::Kind::Failed &&
                event.path == "does-not-exist.kindialogue") {
                dialogue_failed = true;
            }
        }
        assert(dialogue_failed);
    }

    // Deterministic drain order, independent of worker-thread scheduling.
    {
        const auto run_once = [&](int workers) {
            kin::AssetManager run_assets{dir};
            kin::AssetServer server{run_assets, {.worker_count = workers}};
            server.load_async<kin::Image>("tiny.png");
            server.load_async<kin::Image>("tiny.bmp");
            server.load_async<kin::GameInfo>("game.kininfo");
            server.load_async<kin::InputMap>("controls.kininput");
            server.drain();
            std::vector<kin::u64> ids;
            for (const kin::AssetEvent& event : server.events()) {
                ids.push_back(event.request_id);
            }
            return ids;
        };
        const std::vector<kin::u64> ids1 = run_once(1);
        const std::vector<kin::u64> ids4 = run_once(4);
        const std::vector<kin::u64> ids8 = run_once(8);
        assert(ids1.size() == 4);
        assert(ids1 == ids4);
        assert(ids4 == ids8);
        assert(std::is_sorted(ids1.begin(), ids1.end()));
    }

    // Sync load completes an async-reserved storage and shares it.
    {
        kin::AssetManager shared_assets{dir};
        kin::AssetServer server{shared_assets, {.worker_count = 1}};
        kin::AssetHandle<kin::Image> async_handle = server.load_async<kin::Image>("tiny.png");
        assert(!async_handle);
        const std::shared_ptr<const kin::Image> sync = shared_assets.load<kin::Image>("tiny.png");
        assert(sync);
        assert(async_handle); // now valid: same underlying storage
        assert(async_handle.shared() == sync);
        server.drain(); // the queued job re-commits the same storage; still valid
        assert(async_handle);
        assert(async_handle.shared() == sync);
    }

    // ---- Dependency loading: catalog becomes ready only with its textures ----
    {
        kin::AssetManager dep_assets{dir};
        kin::AssetServer server{dep_assets, {.worker_count = 4}};
        kin::register_sprite_catalog_async_loader(server);

        kin::AssetHandle<kin::SpriteCatalog> catalog =
            server.load_async<kin::SpriteCatalog>("hero.kinsprites");
        assert(!catalog);
        assert(!server.ready<kin::SpriteCatalog>("hero.kinsprites"));

        server.drain();

        // The catalog itself loaded...
        assert(catalog);
        assert(catalog->contains("hero.direct"));
        assert(server.load_state<kin::SpriteCatalog>("hero.kinsprites") == kin::LoadState::Loaded);
        // ...and its referenced texture image was auto-loaded as a dependency.
        assert(server.load_state<kin::Image>("tiny.bmp") == kin::LoadState::Loaded);
        // So the whole dependency tree is ready.
        assert(server.ready<kin::SpriteCatalog>("hero.kinsprites"));

        // The parent's Added event precedes the dependency's (parent applied first).
        kin::u64 catalog_id = 0;
        kin::u64 image_id = 0;
        for (const kin::AssetEvent& event : server.events()) {
            if (event.path == "hero.kinsprites") catalog_id = event.request_id;
            if (event.path == "tiny.bmp") image_id = event.request_id;
        }
        assert(catalog_id != 0 && image_id != 0);
        assert(catalog_id < image_id);
    }

    // A loaded asset whose dependency FAILS is itself Loaded but not ready().
    {
        kin::AssetManager gate_assets{dir};
        kin::AssetServer server{gate_assets, {.worker_count = 2}};
        server.register_async_loader<kin::GameInfo>(
            [](const std::filesystem::path& path, kin::LoadContext& ctx) {
                ctx.require<kin::DialogueDocument>("missing.kindialogue"); // will fail
                return kin::load_game_info(path);
            });

        server.load_async<kin::GameInfo>("game.kininfo");
        server.drain();
        assert(server.load_state<kin::GameInfo>("game.kininfo") == kin::LoadState::Loaded);
        assert(server.load_state<kin::DialogueDocument>("missing.kindialogue") == kin::LoadState::Failed);
        assert(!server.ready<kin::GameInfo>("game.kininfo")); // gated on failed dep
    }

    run_anim_asset_loader_tests();

    return 0;
}
