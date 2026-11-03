#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <kin/core/json_value.hpp>
#include <kin/renderer/sprite_catalog.hpp>
#include <kin/scene/scene_preload2d.hpp>
#include <set>
#include <sstream>
#include <stdexcept>

namespace kin {
namespace {
struct Progress {
    std::atomic<std::size_t> read{0}, discovered{1};
};
struct Bundle {
    std::shared_ptr<const MemoryContent> content;
    std::vector<std::pair<std::string, Image>> images;
};
std::string relative(std::string path) {
    const std::filesystem::path p{path};
    if (path.empty() || p.is_absolute() || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos || std::ranges::any_of(p, [](const auto& part) { return part == ".."; }))
        throw std::runtime_error("preload reference must stay inside content: " + path);
    return p.lexically_normal().generic_string();
}
bool image_path(const std::string& name) {
    auto ext = std::filesystem::path{name}.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".bmp" || ext == ".jpg" || ext == ".jpeg" || ext == ".qoi" || ext == ".tga" ||
           ext == ".gif";
}
Bundle read_bundle(const std::filesystem::path& root, const std::string& scene, const ScenePreloadOptions2D& options,
                   const std::shared_ptr<Progress>& progress, std::stop_token stop) {
    MemoryContent::Files files;
    std::set<std::string> seen;
    std::deque<std::string> pending;
    const auto enqueue = [&](const std::string& name) {
        if (name.empty())
            return;
        const auto key = relative(name);
        if (seen.insert(key).second)
            pending.push_back(key);
    };
    enqueue(scene);
    for (const auto& name : options.additional_assets)
        enqueue(name);
    Bundle bundle;
    u64 bytes_read = 0, decoded_bytes = 0;
    while (!pending.empty()) {
        if (stop.stop_requested())
            throw std::runtime_error("scene load cancelled");
        const auto name = pending.front();
        pending.pop_front();
        if (seen.size() > options.max_files)
            throw std::runtime_error("scene preload file limit exceeded");
        const auto size = content_file_size(root / name);
        if (!size || *size > options.max_bytes - std::min(bytes_read, options.max_bytes))
            throw std::runtime_error("missing/oversize preload dependency: " + name);
        auto bytes = read_content_file(root / name);
        if (!bytes)
            throw std::runtime_error("cannot preload dependency: " + name);
        bytes_read += bytes->size();
        if (bytes_read > options.max_bytes)
            throw std::runtime_error("scene preload byte limit exceeded");
        if (image_path(name)) {
            auto image = decode_image({reinterpret_cast<const u8*>(bytes->data()), bytes->size()}, name);
            if (!image.valid())
                throw std::runtime_error("invalid preload image: " + name);
            decoded_bytes += image.rgba.size();
            if (decoded_bytes > options.max_bytes)
                throw std::runtime_error("scene preload decoded image byte limit exceeded");
            bundle.images.emplace_back(name, std::move(image));
        } else if (std::filesystem::path(name).extension() == ".kinsprites") {
            SpriteCatalog catalog;
            std::string error;
            if (!parse_sprite_catalog(*bytes, catalog, error))
                throw std::runtime_error(error);
            for (const auto& [id, texture] : catalog.textures())
                enqueue(texture.path);
        } else if (bytes->find_first_not_of(" \t\r\n") != std::string::npos &&
                   (*bytes)[bytes->find_first_not_of(" \t\r\n")] == '{') {
            const auto parsed = parse_json(*bytes);
            if (!parsed.ok())
                throw std::runtime_error("invalid preload JSON: " + name);
            const auto visit = [&](const auto& self, const JsonValue& value) -> void {
                if (value.is_object())
                    for (const auto& [key, child] : value.members()) {
                        const bool reference = key == "asset" || key == "image" || key == "font" || key == "script" ||
                                               key == "texture" || key == "tileset" || key.ends_with("_asset");
                        if (reference && child.is_string() && !child.as_string().empty())
                            enqueue(child.as_string());
                        else if (reference && child.is_object() && child.find("id") && child.find("id")->is_string())
                            enqueue(child.string_at("id"));
                        else
                            self(self, child);
                    }
                else if (value.is_array())
                    for (const auto& child : value.items())
                        self(self, child);
            };
            visit(visit, *parsed.value);
        } else {
            const auto ext = std::filesystem::path(name).extension().string();
            if (ext.starts_with(".kin") && ext != ".kinscene") {
                std::istringstream tokens(*bytes);
                std::string token;
                while (tokens >> token)
                    if (infer_asset_type(token) != AssetType::Unknown && token.find('#') == std::string::npos)
                        enqueue(token);
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
};
ScenePreload2D::ScenePreload2D(Renderer2D& renderer, JobSystem& jobs, std::filesystem::path root, std::string scene,
                               ScenePreloadOptions2D options)
    : _impl(std::make_unique<Impl>(renderer, jobs)) {
    if (!options.max_files || !options.max_bytes)
        throw std::invalid_argument("scene preload limits must be positive");
    auto& s = *_impl;
    s.source = relative(std::move(scene));
    s.options = std::move(options);
    // Capture CPU-only options, never the renderer-thread scene/request state.
    auto worker_options = s.options;
    worker_options.register_components = {};
    worker_options.asset_binders.clear();
    worker_options.update = {};
    s.job = jobs.run(
        [root = std::move(root), source = s.source, options = std::move(worker_options),
         progress = s.progress](std::stop_token stop) { return read_bundle(root, source, options, progress, stop); });
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
    if (s.state == SceneLoadState2D::Ready || s.state == SceneLoadState2D::Taken)
        return 1;
    if (s.state == SceneLoadState2D::Loading)
        return 0.75f * s.progress->read.load() / std::max<std::size_t>(1, s.progress->discovered.load());
    return 0.75f + 0.25f * s.uploaded / std::max<std::size_t>(1, s.bundle.images.size());
}
void ScenePreload2D::cancel() {
    auto& s = *_impl;
    if (s.state == SceneLoadState2D::Taken)
        return;
    s.job.cancel();
    s.scene.reset();
    s.bundle = {};
    s.state = SceneLoadState2D::Cancelled;
}
void ScenePreload2D::pump(std::size_t texture_budget) {
    auto& s = *_impl;
    if (s.state != SceneLoadState2D::Loading && s.state != SceneLoadState2D::Uploading)
        return;
    try {
        if (s.state == SceneLoadState2D::Loading) {
            if (s.jobs.worker_count() == 0)
                s.jobs.pump();
            if (!s.job.ready())
                return;
            s.bundle = std::move(s.job.get());
            s.scene = std::make_unique<PreloadedScene2D>();
            s.scene->_content = s.bundle.content;
            s.scene->_update = s.options.update;
            register_builtin_editor_components(s.scene->ecs());
            if (s.options.register_components)
                s.options.register_components(s.scene->ecs());
            auto loaded = load_scene_asset(s.bundle.content->root() / s.source, s.scene->ecs().components());
            if (!loaded.ok())
                throw std::runtime_error(loaded.diagnostics.empty() ? "invalid scene" : loaded.diagnostics.front());
            s.scene->_definition = std::move(*loaded.scene);
            s.scene->_instance =
                instantiate_scene_asset(s.scene->ecs(), s.scene->_definition, s.bundle.content->root(),
                                        s.scene->ecs().components(), {}, {.update_scene_header = true});
            if (!s.scene->_instance.ok())
                throw std::runtime_error(s.scene->_instance.diagnostics.front());
            s.scene->render_config().enabled = true;
            s.scene->render_config().clear_color = s.scene->_definition.clear_color;
            for (const auto& [name, binder] : s.options.asset_binders)
                s.scene->add_asset_binder(name, binder);
            s.scene->adopt_assets(std::make_unique<Assets2D>(s.renderer, s.bundle.content->root()));
            s.state = SceneLoadState2D::Uploading;
        }
        for (std::size_t count = 0; count < texture_budget && s.uploaded < s.bundle.images.size(); ++count) {
            auto& [path, pixels] = s.bundle.images[s.uploaded];
            if (!s.scene->assets()->prime_texture(path, pixels))
                throw std::runtime_error("texture upload failed: " + path);
            pixels = {};
            ++s.uploaded;
        }
        if (s.uploaded == s.bundle.images.size()) {
            s.scene->assets()->bind(s.scene->ecs());
            if (!s.scene->assets()->problems().empty())
                throw std::runtime_error(s.scene->assets()->problems().begin()->second);
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
    if (_impl->state != SceneLoadState2D::Ready)
        return {};
    _impl->state = SceneLoadState2D::Taken;
    _impl->bundle = {};
    return std::move(_impl->scene);
}
} // namespace kin
