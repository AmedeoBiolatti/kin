#pragma once

#include <kin/renderer/render_queue.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

namespace render_pass_id {
constexpr std::string_view world = "world";
constexpr std::string_view effects = "effects";
constexpr std::string_view ui = "ui";
constexpr std::string_view debug = "debug";
constexpr std::string_view material_albedo = "material_albedo";
constexpr std::string_view material_normal = "material_normal";
constexpr std::string_view material_surface = "material_surface";
constexpr std::string_view lighting = "lighting";
constexpr std::string_view shadow = "shadow";
constexpr std::string_view post = "post";
constexpr std::string_view editor_overlay = "editor_overlay";
} // namespace render_pass_id

struct RenderPassId {
    std::string value;

    friend bool operator==(const RenderPassId& a, const RenderPassId& b) {
        return a.value == b.value;
    }
};

struct RenderPassStats {
    u64 frames = 0;
    u64 commands = 0;
    f64 last_ms = 0.0;
    f64 total_ms = 0.0;
};

struct RenderPassContext {
    Renderer2D& renderer;
    RenderQueue& queue;
    const RenderView* view = nullptr;
};

struct RenderPass {
    RenderPassId id;
    std::string name;
    bool enabled = true;
    u64 mask = render_pass_mask::all;
    std::vector<RenderPassId> dependencies;
    std::function<void(RenderPassContext&)> run;
    RenderPassStats stats{};
};

class RenderPassRegistry {
public:
    RenderPass& add(RenderPass pass);
    bool remove(std::string_view id);
    void clear();
    RenderPass* find(std::string_view id);
    const RenderPass* find(std::string_view id) const;
    bool set_enabled(std::string_view id, bool enabled);
    bool enabled(std::string_view id) const;
    std::vector<RenderPass>& passes() { return _passes; }
    const std::vector<RenderPass>& passes() const { return _passes; }

private:
    std::vector<RenderPass> _passes;
};

class RenderGraph {
public:
    RenderPassRegistry& passes() { return _passes; }
    const RenderPassRegistry& passes() const { return _passes; }

    void clear();
    void add_default_passes();
    void execute(Renderer2D& renderer, RenderQueue& queue, const RenderView* view = nullptr);

private:
    RenderPassRegistry _passes;
};

} // namespace kin
