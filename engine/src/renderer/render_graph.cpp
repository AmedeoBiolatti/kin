#include <kin/renderer/render_graph.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_set>

namespace kin {
namespace {

bool id_equals(const RenderPass& pass, std::string_view id) {
    return pass.id.value == id;
}

} // namespace

RenderPass& RenderPassRegistry::add(RenderPass pass) {
    if (pass.id.value.empty()) {
        throw std::runtime_error("Render pass requires a stable id");
    }
    if (pass.name.empty()) {
        pass.name = pass.id.value;
    }
    if (RenderPass* existing = find(pass.id.value)) {
        *existing = std::move(pass);
        return *existing;
    }
    _passes.push_back(std::move(pass));
    return _passes.back();
}

bool RenderPassRegistry::remove(std::string_view id) {
    const auto old_size = _passes.size();
    std::erase_if(_passes, [&](const RenderPass& pass) {
        return pass.id.value == id;
    });
    return _passes.size() != old_size;
}

void RenderPassRegistry::clear() {
    _passes.clear();
}

RenderPass* RenderPassRegistry::find(std::string_view id) {
    const auto found = std::ranges::find_if(_passes, [&](const RenderPass& pass) {
        return id_equals(pass, id);
    });
    return found == _passes.end() ? nullptr : &*found;
}

const RenderPass* RenderPassRegistry::find(std::string_view id) const {
    const auto found = std::ranges::find_if(_passes, [&](const RenderPass& pass) {
        return id_equals(pass, id);
    });
    return found == _passes.end() ? nullptr : &*found;
}

bool RenderPassRegistry::set_enabled(std::string_view id, bool enabled_value) {
    RenderPass* pass = find(id);
    if (!pass) {
        return false;
    }
    pass->enabled = enabled_value;
    return true;
}

bool RenderPassRegistry::enabled(std::string_view id) const {
    const RenderPass* pass = find(id);
    return pass ? pass->enabled : false;
}

void RenderGraph::clear() {
    _passes.clear();
}

void RenderGraph::add_default_passes() {
    _passes.add({
        .id = {std::string{render_pass_id::world}},
        .name = "World",
        .mask = render_pass_mask::world,
        .run = [](RenderPassContext& ctx) {
            ctx.view ? ctx.queue.flush(ctx.renderer, *ctx.view, render_pass_mask::world)
                     : ctx.queue.flush(ctx.renderer, render_pass_mask::world);
        },
    });
    _passes.add({
        .id = {std::string{render_pass_id::effects}},
        .name = "Effects",
        .mask = render_pass_mask::effects,
        .run = [](RenderPassContext& ctx) {
            ctx.view ? ctx.queue.flush(ctx.renderer, *ctx.view, render_pass_mask::effects)
                     : ctx.queue.flush(ctx.renderer, render_pass_mask::effects);
        },
    });
    _passes.add({
        .id = {std::string{render_pass_id::ui}},
        .name = "UI",
        .mask = render_pass_mask::ui,
        .run = [](RenderPassContext& ctx) {
            ctx.queue.flush(ctx.renderer, render_pass_mask::ui);
        },
    });
    _passes.add({
        .id = {std::string{render_pass_id::debug}},
        .name = "Debug",
        .mask = render_pass_mask::debug,
        .run = [](RenderPassContext& ctx) {
            ctx.queue.flush(ctx.renderer, render_pass_mask::debug);
        },
    });
}

void RenderGraph::execute(Renderer2D& renderer, RenderQueue& queue, const RenderView* view) {
    std::unordered_set<std::string> executed;
    const auto run_pass = [&](auto& self, RenderPass& pass) -> void {
        if (executed.contains(pass.id.value)) {
            return;
        }
        for (const RenderPassId& dependency : pass.dependencies) {
            if (RenderPass* dep = _passes.find(dependency.value)) {
                self(self, *dep);
            }
        }
        executed.insert(pass.id.value);
        if (!pass.enabled) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        if (pass.run) {
            RenderPassContext ctx{renderer, queue, view};
            pass.run(ctx);
        }
        const auto end = std::chrono::steady_clock::now();
        pass.stats.frames += 1;
        pass.stats.commands = queue.size();
        pass.stats.last_ms = std::chrono::duration<f64, std::milli>(end - start).count();
        pass.stats.total_ms += pass.stats.last_ms;
    };

    for (RenderPass& pass : _passes.passes()) {
        run_pass(run_pass, pass);
    }
}

} // namespace kin
