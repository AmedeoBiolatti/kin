#include <kin/scene/scene_preload2d.hpp>

#include <kin/assets/asset_manager.hpp>
#include <kin/assets/image.hpp>
#include <kin/core/json_value.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <set>
#include <sstream>
#include <stdexcept>

namespace kin {
namespace {

constexpr std::size_t max_preload_files = 1024;
constexpr u64 max_preload_bytes = 256ull * 1024 * 1024;

struct Progress {
    std::atomic<std::size_t> read{0};
    std::atomic<std::size_t> discovered{1};
};

struct Bundle {
    std::shared_ptr<const MemoryContent> content;
    std::vector<std::pair<std::string, Image>> images;
};

std::string relative(std::string path) {
    const std::filesystem::path p{path};
    if (path.empty() || p.is_absolute() || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos || std::ranges::any_of(p, [](const auto& part) { return part == ".."; })) {
        throw std::runtime_error("preload reference must stay inside content: " + path);
    }
    return p.lexically_normal().generic_string();
}

// An image the preload decodes on its job (compressed textures load as files).
bool decoded_image(const std::string& name) {
    if (infer_asset_type(name) != AssetType::Image) {
        return false;
    }
    auto ext = std::filesystem::path{name}.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext != ".ktx2" && ext != ".dds";
}

Bundle read_bundle(const std::filesystem::path& root, const std::string& scene,
                   const std::shared_ptr<Progress>& progress, std::stop_token stop) {
    MemoryContent::Files files;
    std::set<std::string> seen;
    std::deque<std::string> pending;
    const auto enqueue = [&](const std::string& name) {
        if (name.empty()) {
            return;
        }
        const auto key = relative(name);
        if (seen.insert(key).second) {
            pending.push_back(key);
        }
    };
    enqueue(scene);
    Bundle bundle;
    u64 bytes_read = 0;
    u64 decoded_bytes = 0;
    while (!pending.empty()) {
        if (stop.stop_requested()) {
            throw std::runtime_error("scene load cancelled");
        }
        const auto name = pending.front();
        pending.pop_front();
        if (seen.size() > max_preload_files) {
            throw std::runtime_error("scene preload file limit exceeded");
        }
        const auto size = content_file_size(root / name);
        if (!size || *size > max_preload_bytes - bytes_read) {
            throw std::runtime_error("missing/oversize preload dependency: " + name);
        }
        auto bytes = read_content_file(root / name);
        if (!bytes) {
            throw std::runtime_error("cannot preload dependency: " + name);
        }
        bytes_read += bytes->size();
        if (bytes_read > max_preload_bytes) {
            throw std::runtime_error("scene preload byte limit exceeded");
        }
        const auto first = bytes->find_first_not_of(" \t\r\n");
        if (decoded_image(name)) {
            auto image = decode_image({reinterpret_cast<const u8*>(bytes->data()), bytes->size()}, name);
            if (!image.valid()) {
                throw std::runtime_error("invalid preload image: " + name);
            }
            decoded_bytes += image.rgba.size();
            if (decoded_bytes > max_preload_bytes) {
                throw std::runtime_error("scene preload decoded image byte limit exceeded");
            }
            bundle.images.emplace_back(name, std::move(image));
        } else if (std::filesystem::path(name).extension() == ".kinsprites") {
            SpriteCatalog catalog;
            std::string error;
            if (!parse_sprite_catalog(*bytes, catalog, error)) {
                throw std::runtime_error(error);
            }
            for (const auto& [id, texture] : catalog.textures()) {
                enqueue(texture.path);
            }
        } else if (first != std::string::npos && (*bytes)[first] == '{') {
            const auto parsed = parse_json(*bytes);
            if (!parsed.ok()) {
                throw std::runtime_error("invalid preload JSON: " + name);
            }
            // Asset references by key name, at any depth.
            const auto visit = [&](const auto& self, const JsonValue& value) -> void {
                if (value.is_object()) {
                    for (const auto& [key, child] : value.members()) {
                        const bool reference = key == "asset" || key == "image" || key == "font" || key == "script" ||
                                               key == "texture" || key == "tileset" || key.ends_with("_asset");
                        if (reference && child.is_string() && !child.as_string().empty()) {
                            enqueue(child.as_string());
                        } else if (reference && child.is_object() && child.find("id") &&
                                   child.find("id")->is_string()) {
                            enqueue(child.string_at("id"));
                        } else {
                            self(self, child);
                        }
                    }
                } else if (value.is_array()) {
                    for (const auto& child : value.items()) {
                        self(self, child);
                    }
                }
            };
            visit(visit, *parsed.value);
        } else {
            const auto ext = std::filesystem::path(name).extension().string();
            if (ext.starts_with(".kin") && ext != ".kinscene") {
                std::istringstream tokens(*bytes);
                std::string token;
                while (tokens >> token) {
                    if (infer_asset_type(token) != AssetType::Unknown && token.find('#') == std::string::npos) {
                        enqueue(token);
                    }
                }
            }
        }
        files.emplace(name, std::move(*bytes));
        progress->discovered.store(seen.size());
        progress->read.fetch_add(1);
    }
    bundle.content = MemoryContent::create(std::move(files));
    return bundle;
}

} // namespace

struct ScenePreload2D::Impl {
    Renderer2D& renderer;
    JobSystem& jobs;
    ScenePreloadOptions2D options;
    std::string source;
    std::shared_ptr<Progress> progress = std::make_shared<Progress>();
    Job<Bundle> job;
    Bundle bundle;
    std::unique_ptr<PreloadedScene2D> scene;
    SceneLoadState2D state = SceneLoadState2D::Loading;
    std::size_t uploaded = 0;
    std::string error;

    Impl(Renderer2D& r, JobSystem& j) : renderer(r), jobs(j) {}

    // The bundle read: reflect and instantiate the scene, give it its assets.
    void make_scene() {
        scene = std::make_unique<PreloadedScene2D>();
        scene->_content = bundle.content;
        register_builtin_editor_components(scene->ecs());
        const std::filesystem::path root = bundle.content->root();
        auto loaded = load_scene_asset(root / source, scene->ecs().components());
        if (!loaded.ok()) {
            throw std::runtime_error(loaded.diagnostics.empty() ? "invalid scene" : loaded.diagnostics.front());
        }
        scene->_definition = std::move(*loaded.scene);
        auto instance = instantiate_scene_asset(scene->ecs(), scene->_definition, root, scene->ecs().components(), {},
                                                {.update_scene_header = true});
        if (!instance.ok()) {
            throw std::runtime_error(instance.diagnostics.front());
        }
        scene->render_config().enabled = true;
        scene->render_config().clear_color = scene->_definition.clear_color;
        scene->_runtime = std::make_unique<SceneAssetRuntime>();
        scene->_runtime->track_instance(root / source, root, scene->_definition, std::move(instance));
        for (const auto& [name, binder] : options.asset_binders) {
            scene->add_asset_binder(name, binder);
        }
        scene->adopt_assets(std::make_unique<Assets2D>(renderer, root));
    }
};

std::unique_ptr<DevelopmentChange> PreloadedScene2D::prepare_reload(const DevelopmentRevision& revision,
                                                                    std::string& error) {
    // The snapshot never changes, so the scene as loaded can be read again now.
    if (!_runtime->tracks_authored() && !_runtime->track_authored(ecs().components())) {
        error = _runtime->diagnostics().empty() ? "cannot read the scene as loaded" : _runtime->diagnostics().front();
        return {};
    }
    return prepare_asset_reload(
        revision,
        [&](const AssetPreviewCheck& check, std::string& failure) {
            return _runtime->prepare_patch(ecs(), revision.content->root(), failure, check);
        },
        [this, content = revision.content] {
            _content = content;
            _definition = *_runtime->scene();
            render_config().clear_color = _definition.clear_color;
            _preloaded.clear();
        },
        error);
}

ScenePreload2D::ScenePreload2D(Renderer2D& renderer, JobSystem& jobs, std::filesystem::path root, std::string scene,
                               ScenePreloadOptions2D options)
    : _impl(std::make_unique<Impl>(renderer, jobs)) {
    auto& s = *_impl;
    s.source = relative(std::move(scene));
    s.options = std::move(options);
    // The job reads files only; the scene and its assets are made in pump().
    const auto source_owner = memory_content_for(root);
    s.job = jobs.run(
        [root = std::move(root), source = s.source, progress = s.progress, source_owner](std::stop_token stop) {
            (void)source_owner;
            return read_bundle(root, source, progress, stop);
        });
}

ScenePreload2D::~ScenePreload2D() {
    _impl->job.cancel();
}

SceneLoadState2D ScenePreload2D::state() const {
    return _impl->state;
}

const std::string& ScenePreload2D::error() const {
    return _impl->error;
}

f32 ScenePreload2D::progress() const {
    const auto& s = *_impl;
    if (s.state == SceneLoadState2D::Ready || s.state == SceneLoadState2D::Taken) {
        return 1;
    }
    if (s.state == SceneLoadState2D::Loading) {
        return 0.75f * s.progress->read.load() / std::max<std::size_t>(1, s.progress->discovered.load());
    }
    return 0.75f + 0.25f * s.uploaded / std::max<std::size_t>(1, s.bundle.images.size());
}

void ScenePreload2D::cancel() {
    auto& s = *_impl;
    if (s.state == SceneLoadState2D::Taken) {
        return;
    }
    s.job.cancel();
    s.scene.reset();
    s.bundle = {};
    s.state = SceneLoadState2D::Cancelled;
}

void ScenePreload2D::pump(std::size_t texture_budget) {
    auto& s = *_impl;
    if (s.state != SceneLoadState2D::Loading && s.state != SceneLoadState2D::Uploading) {
        return;
    }
    try {
        if (s.state == SceneLoadState2D::Loading) {
            if (s.jobs.worker_count() == 0) {
                s.jobs.pump();
            }
            if (!s.job.ready()) {
                return;
            }
            s.bundle = std::move(s.job.get());
            s.make_scene();
            s.state = SceneLoadState2D::Uploading;
        }
        Assets2D& assets = *s.scene->assets();
        for (std::size_t count = 0; count < texture_budget && s.uploaded < s.bundle.images.size(); ++count) {
            auto& [path, pixels] = s.bundle.images[s.uploaded];
            if (!assets.prime_texture(path, pixels)) {
                throw std::runtime_error("texture upload failed: " + path);
            }
            s.scene->_preloaded.load<Image>(assets.manager(), path);
            pixels = {};
            ++s.uploaded;
        }
        if (s.uploaded == s.bundle.images.size()) {
            assets.bind(s.scene->ecs());
            if (const auto problem = assets.first_problem()) {
                throw std::runtime_error(*problem);
            }
            s.state = SceneLoadState2D::Ready;
        }
    } catch (const std::exception& error) {
        s.error = error.what();
        s.state = SceneLoadState2D::Failed;
        s.scene.reset();
        s.bundle = {};
    }
}

std::unique_ptr<PreloadedScene2D> ScenePreload2D::take() {
    if (_impl->state != SceneLoadState2D::Ready) {
        return {};
    }
    _impl->state = SceneLoadState2D::Taken;
    _impl->bundle = {};
    return std::move(_impl->scene);
}

} // namespace kin
