#include <algorithm>
#include <atomic>
#include <chrono>
#include <kin/renderer/resource_graph.hpp>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace kin {
u64 graph_resource_payload(const GraphResourceDesc& desc) {
    if (desc.kind == GraphResourceKind::Token)
        return 0;
    if (desc.kind == GraphResourceKind::Buffer)
        return desc.bytes;
    const u64 single = texture_level_bytes(desc.format, desc.size);
    const u64 layers = std::max(1u, desc.array_layers);
    const u64 bytes = single > ~u64{0} / layers ? ~u64{0} : single * layers;
    if (!desc.hdr)
        return bytes;
    return bytes > std::numeric_limits<u64>::max() / 2 ? std::numeric_limits<u64>::max() : bytes * 2;
}
namespace {
bool compatible(const GraphResourceDesc& a, const GraphResourceDesc& b) {
    return a.kind == b.kind && a.format == b.format && a.size == b.size && a.bytes == b.bytes && a.hdr == b.hdr &&
           a.array_layers == b.array_layers;
}
CompiledResourceGraph build(const ResourceGraphDesc& graph, u64 features, GraphCompileOptions options) {
    CompiledResourceGraph plan;
    plan.source = graph;
    const u32 count = static_cast<u32>(graph.passes.size()), resources = static_cast<u32>(graph.resources.size());
    plan.lifetimes.resize(resources);
    auto error = [&](std::string message) { plan.diagnostics.push_back(std::move(message)); };
    std::unordered_set<std::string> resource_names;
    for (const auto& r : graph.resources) {
        if (r.name.empty() || !resource_names.insert(r.name).second)
            error("empty or duplicate resource name: " + r.name);
        if (r.kind != GraphResourceKind::Token && r.kind != GraphResourceKind::Buffer &&
            texture_level_bytes(r.format, r.size) == ~u64{0})
            error(r.name + ": texture payload overflows the planner");
        if (r.array_layers && !(features & graph_feature::attachment_arrays))
            error(r.name + ": attachment arrays unavailable on backend");
        if (r.array_layers && r.kind != GraphResourceKind::RenderAttachment)
            error(r.name + ": arrays require explicit render attachments");
        if (r.array_layers && graph_resource_payload(r) == ~u64{0})
            error(r.name + ": array payload overflows the planner");
        if (r.hdr && r.kind != GraphResourceKind::ColorTarget)
            error(r.name + ": HDR applies to colour targets only");
        if (r.hdr && texture_level_bytes(r.format, r.size) > std::numeric_limits<u64>::max() / 2)
            error(r.name + ": texture payload overflows the planner");
        if (r.kind == GraphResourceKind::ColorTarget && r.format != TextureFormat::Rgba8)
            error(r.name + ": public color targets currently require Rgba8");
        if (r.kind != GraphResourceKind::Token && r.kind != GraphResourceKind::Buffer &&
            (r.size.x <= 0 || r.size.y <= 0))
            error(r.name + ": texture extent must be positive");
        if (r.kind == GraphResourceKind::Buffer && (!r.bytes || r.bytes > std::numeric_limits<u32>::max()))
            error(r.name + ": buffer capacity must fit a nonzero SDL_GPU buffer");
        if (r.kind == GraphResourceKind::StorageTexture && texture_format_compressed(r.format))
            error(r.name + ": compressed storage textures are unsupported");
        if (r.kind == GraphResourceKind::StorageTexture &&
            (r.format == TextureFormat::Depth32Float || r.format == TextureFormat::Depth24Unorm))
            error(r.name + ": depth storage textures are unsupported");
        if (r.kind == GraphResourceKind::RenderAttachment && r.format != TextureFormat::Rg16Float &&
            r.format != TextureFormat::Rgba16Float && r.format != TextureFormat::Depth32Float &&
            r.format != TextureFormat::Depth24Unorm)
            error(r.name + ": render attachments require RG16F, RGBA16F, D32F or D24");
    }
    std::unordered_map<std::string, u32> names;
    std::vector<u32> writer(resources, graph_no_slot);
    std::vector<bool> discarded(resources);
    std::vector<std::vector<u32>> reads(count), writes(count);
    std::vector<u32> load_previous(resources, graph_no_slot), load_pass(resources, graph_no_slot);
    const auto is_depth = [](const GraphResourceDesc& r) {
        return r.kind == GraphResourceKind::RenderAttachment &&
               (r.format == TextureFormat::Depth32Float || r.format == TextureFormat::Depth24Unorm);
    };
    std::vector<std::vector<u32>> dependencies(count), consumers(count);
    for (u32 i = 0; i < count; ++i) {
        const auto& pass = graph.passes[i];
        if (pass.name.empty() || !names.emplace(pass.name, i).second)
            error("empty or duplicate pass name: " + pass.name);
        if (!pass.enabled)
            continue;
        reads[i] = pass.reads;
        writes[i] = pass.writes;
        std::unordered_set<u32> attached;
        u32 depth_count = 0;
        for (const auto& a : pass.attachments) {
            if (a.resource >= resources) {
                error(pass.name + ": invalid attachment resource");
                continue;
            }
            const auto& r = graph.resources[a.resource];
            if (r.kind != GraphResourceKind::ColorTarget && r.kind != GraphResourceKind::RenderAttachment)
                error(pass.name + ": attachments require colour/depth targets");
            if (!attached.insert(a.resource).second)
                error(pass.name + ": duplicate attachment");
            if (is_depth(r) && ++depth_count > 1)
                error(pass.name + ": multiple depth attachments");
            if (std::ranges::find(pass.reads, a.resource) != pass.reads.end())
                error(pass.name + ": attached resource cannot also be sampled");
            if (a.load != GraphAttachmentLoad::Clear && a.load != GraphAttachmentLoad::Discard &&
                a.load != GraphAttachmentLoad::Load)
                error(pass.name + ": invalid attachment load operation");
            if (a.store != GraphAttachmentStore::Store && a.store != GraphAttachmentStore::Discard)
                error(pass.name + ": invalid attachment store operation");
            if (a.access == GraphAttachmentAccess::ReadOnlyDepth) {
                if (!is_depth(r) || a.load != GraphAttachmentLoad::Load || a.store != GraphAttachmentStore::Store ||
                    a.previous != graph_no_slot)
                    error(pass.name + ": read-only depth requires existing stored depth, Load and no previous version");
                reads[i].push_back(a.resource);
                continue;
            }
            if (a.access != GraphAttachmentAccess::Write)
                error(pass.name + ": invalid attachment access");
            writes[i].push_back(a.resource);
            discarded[a.resource] = a.store == GraphAttachmentStore::Discard;
            if (a.load == GraphAttachmentLoad::Load) {
                if (a.previous >= resources || a.previous == a.resource) {
                    error(pass.name + ": attachment Load requires a distinct previous resource");
                    continue;
                }
                if (!compatible(r, graph.resources[a.previous]))
                    error(pass.name + ": incompatible attachment versions");
                reads[i].push_back(a.previous);
                load_previous[a.resource] = a.previous;
                load_pass[a.resource] = i;
            } else if (a.previous != graph_no_slot)
                error(pass.name + ": previous version requires attachment Load");
        }
        std::unordered_set<u32> used;
        for (auto id : writes[i]) {
            if (id >= resources) {
                error(pass.name + ": invalid write resource");
                continue;
            }
            if (!used.insert(id).second)
                error(pass.name + ": duplicate write declaration");
            if (graph.resources[id].imported)
                error(pass.name + ": imported values are read-only; write a new version");
            if (writer[id] != graph_no_slot)
                error(graph.resources[id].name + ": multiple writers; use distinct versions");
            writer[id] = i;
        }
        used.clear();
        for (auto id : reads[i]) {
            if (id >= resources) {
                error(pass.name + ": invalid read resource");
                continue;
            }
            // Loading an attachment may also sample its previous version. That
            // is legal with a copy, but never with in-place storage.
            if (!used.insert(id).second && std::ranges::count(pass.reads, id) > 1)
                error(pass.name + ": duplicate read declaration");
            if (std::ranges::find(writes[i], id) != writes[i].end())
                error(pass.name + ": feedback requires ping-pong resources");
        }
    }
    if (!plan)
        return plan;
    const auto edge = [&](u32 from, u32 to) {
        if (std::ranges::find(dependencies[to], from) == dependencies[to].end()) {
            dependencies[to].push_back(from);
            consumers[from].push_back(to);
        }
    };
    for (u32 i = 0; i < count; ++i) {
        const auto& pass = graph.passes[i];
        if (!pass.enabled)
            continue;
        for (auto id : reads[i]) {
            if (discarded[id])
                error(pass.name + ": reading discarded attachment contents: " + graph.resources[id].name);
            if (writer[id] != graph_no_slot)
                edge(writer[id], i);
            else if (!graph.resources[id].imported)
                error(pass.name + ": read before initialization: " + graph.resources[id].name);
        }
        for (const auto& name : pass.after) {
            auto found = names.find(name);
            if (found == names.end() || !graph.passes[found->second].enabled)
                error(pass.name + ": missing or disabled dependency: " + name);
            else
                edge(found->second, i);
        }
    }
    for (u32 id = 0; id < resources; ++id) {
        if (graph.resources[id].exported && discarded[id])
            error(graph.resources[id].name + ": cannot export discarded attachment contents");
        if (graph.resources[id].exported && !graph.resources[id].imported && writer[id] == graph_no_slot)
            error(graph.resources[id].name + ": exported value has no writer");
    }
    if (!plan)
        return plan;
    // Iterative traversal and Kahn sorting keep deep user graphs off the C++ stack.
    std::vector<bool> retained(count, !options.cull_unused);
    std::vector<u32> pending;
    for (u32 i = 0; i < count; ++i)
        if (graph.passes[i].enabled && graph.passes[i].side_effect)
            pending.push_back(i);
    for (u32 id = 0; id < resources; ++id)
        if (graph.resources[id].exported && writer[id] != graph_no_slot)
            pending.push_back(writer[id]);
    while (!pending.empty()) {
        const u32 i = pending.back();
        pending.pop_back();
        if (retained[i])
            continue;
        retained[i] = true;
        for (auto dep : dependencies[i])
            pending.push_back(dep);
    }
    std::vector<u32> degree(count);
    std::priority_queue<u32, std::vector<u32>, std::greater<u32>> ready;
    u32 enabled = 0;
    for (u32 i = 0; i < count; ++i)
        if (graph.passes[i].enabled) {
            degree[i] = static_cast<u32>(dependencies[i].size());
            ++enabled;
            if (!degree[i])
                ready.push(i);
        }
    u32 visited = 0;
    while (!ready.empty()) {
        const u32 next = ready.top();
        ready.pop();
        ++visited;
        for (auto child : consumers[next])
            if (!--degree[child])
                ready.push(child);
        if (!retained[next]) {
            ++plan.culled_passes;
            continue;
        }
        if ((graph.passes[next].required_features & features) != graph.passes[next].required_features)
            error(graph.passes[next].name + ": required backend features unavailable");
        plan.order.push_back(next);
    }
    if (visited != enabled) {
        error("render graph dependency cycle");
        return plan;
    }
    for (u32 position = 0; position < plan.order.size(); ++position) {
        const auto& pass = graph.passes[plan.order[position]];
        const auto touch = [&](u32 id) {
            auto& life = plan.lifetimes[id];
            life.first = std::min(life.first, position);
            life.last = position;
        };
        for (auto id : reads[plan.order[position]])
            touch(id);
        for (auto id : writes[plan.order[position]])
            touch(id);
    }
    for (u32 id = 0; id < resources; ++id)
        if (graph.resources[id].imported && graph.resources[id].exported && plan.lifetimes[id].first == graph_no_slot)
            plan.lifetimes[id] = {.first = 0, .last = static_cast<u32>(plan.order.size())};
    std::vector<u32> ordered;
    for (u32 id = 0; id < resources; ++id)
        if (plan.lifetimes[id].first != graph_no_slot) {
            if (graph.resources[id].exported || graph.resources[id].persistent)
                plan.lifetimes[id].last = static_cast<u32>(plan.order.size());
            ordered.push_back(id);
        }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](u32 a, u32 b) { return plan.lifetimes[a].first < plan.lifetimes[b].first; });
    std::vector<u32> last, owner;
    std::vector<bool> pinned;
    for (auto id : ordered) {
        const auto& desc = graph.resources[id];
        auto& life = plan.lifetimes[id];
        if (desc.imported || desc.kind == GraphResourceKind::Token)
            continue;
        const u64 needed = desc.kind == GraphResourceKind::ColorTarget        ? graph_feature::raster
                           : desc.kind == GraphResourceKind::Buffer           ? graph_feature::buffers
                           : desc.kind == GraphResourceKind::RenderAttachment ? graph_feature::attachments
                                                                              : graph_feature::compute;
        if ((features & needed) != needed)
            error(desc.name + ": resource kind unavailable on backend");
        u32 slot = graph_no_slot;
        const auto previous = load_previous[id];
        if (previous != graph_no_slot) {
            const auto& source = graph.resources[previous];
            const auto& old = plan.lifetimes[previous];
            const auto& pass = graph.passes[load_pass[id]];
            std::string reason;
            if (!options.reuse_slots)
                reason = "slot reuse disabled";
            else if (source.imported)
                reason = "previous version is imported";
            else if (source.exported || source.persistent || desc.persistent)
                reason = "version is pinned";
            else if (std::ranges::find(pass.reads, previous) != pass.reads.end())
                reason = "previous version is sampled by this pass";
            else if (old.last != life.first)
                reason = "previous version has later readers";
            else if (old.slot == graph_no_slot || owner[old.slot] != previous || pinned[old.slot])
                reason = "previous slot is unavailable for handoff";
            else {
                slot = old.slot;
                reason = "last attachment use; storage handed to next version";
                ++plan.attachment_reuses;
            }
            plan.attachment_loads.push_back({load_pass[id], id, previous, slot != graph_no_slot, std::move(reason)});
            if (slot == graph_no_slot) {
                if (!(features & graph_feature::texture_copies))
                    error(desc.name + ": attachment Load requires texture copy support for preserved versions");
                const auto bytes = graph_resource_payload(desc);
                if (bytes > ~u64{0} - plan.attachment_copy_bytes)
                    error("attachment copy payload overflows the planner");
                else
                    plan.attachment_copy_bytes += bytes;
                plan.attachment_copies.push_back({load_pass[id], previous, id, bytes});
            }
        }
        if (slot == graph_no_slot && options.reuse_slots && !desc.exported && !desc.persistent)
            for (u32 s = 0; s < plan.slots.size(); ++s)
                if (!pinned[s] && last[s] < life.first && compatible(plan.slots[s], desc)) {
                    slot = s;
                    break;
                }
        if (slot == graph_no_slot) {
            slot = static_cast<u32>(plan.slots.size());
            plan.slots.push_back(desc);
            last.push_back(0);
            owner.push_back(graph_no_slot);
            pinned.push_back(desc.exported || desc.persistent);
            const u64 bytes = graph_resource_payload(desc);
            if (bytes > std::numeric_limits<u64>::max() - plan.slot_payload_bytes)
                error("graph payload overflows the planner");
            else
                plan.slot_payload_bytes += bytes;
        }
        life.slot = slot;
        last[slot] = life.last;
        owner[slot] = id;
        pinned[slot] = pinned[slot] || desc.exported || desc.persistent;
    }
    std::vector<u32> position(count, graph_no_slot);
    for (u32 i = 0; i < plan.order.size(); ++i)
        position[plan.order[i]] = i;
    std::stable_sort(plan.attachment_copies.begin(), plan.attachment_copies.end(),
                     [&](const auto& a, const auto& b) { return position[a.before_pass] < position[b.before_pass]; });
    if (plan.slot_payload_bytes > options.max_slot_payload_bytes)
        error("graph-managed resource payload exceeds the requested budget");
    if (!plan)
        return plan;
    std::vector<u64> begins(plan.order.size() + 2), ends(plan.order.size() + 2);
    for (auto id : ordered)
        if (!graph.resources[id].imported) {
            const auto bytes = graph_resource_payload(graph.resources[id]);
            auto& begin = begins[plan.lifetimes[id].first];
            auto& end = ends[plan.lifetimes[id].last + 1];
            if (bytes > ~u64{0} - begin || bytes > ~u64{0} - end) {
                error("logical graph payload overflows the planner");
                return plan;
            }
            begin += bytes;
            end += bytes;
        }
    u64 live = 0;
    for (u32 position = 0; position <= plan.order.size(); ++position) {
        live -= ends[position];
        if (begins[position] > ~u64{0} - live) {
            error("logical graph payload overflows the planner");
            return plan;
        }
        live += begins[position];
        plan.peak_live_payload_bytes = std::max(plan.peak_live_payload_bytes, live);
    }
    return plan;
}
} // namespace
const CompiledResourceGraph& ResourceGraphCompiler::compile(const ResourceGraphDesc& source, u64 features,
                                                            GraphCompileOptions options) {
    if (_compiled && _features == features && _options == options && _plan.source == source) {
        ++_stats.cache_hits;
        return _plan;
    }
    const auto start = std::chrono::steady_clock::now();
    auto plan = build(source, features, options);
    static std::atomic<u64> next_identity{1};
    plan.identity = next_identity.fetch_add(1, std::memory_order_relaxed);
    _features = features;
    _options = options;
    _plan = std::move(plan);
    _compiled = true;
    ++_stats.compilations;
    _stats.last_compile_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    return _plan;
}
bool execute_resource_graph(const CompiledResourceGraph& plan, std::span<const std::function<bool()>> operations,
                            const std::function<bool(const GraphAttachmentCopy&)>& copy) {
    if (!plan || (!plan.attachment_copies.empty() && !copy))
        return false;
    for (auto pass : plan.order)
        if (pass >= operations.size() || !operations[pass])
            return false;
    std::size_t next_copy = 0;
    for (auto pass : plan.order) {
        while (next_copy < plan.attachment_copies.size() && plan.attachment_copies[next_copy].before_pass == pass)
            if (!copy(plan.attachment_copies[next_copy++]))
                return false;
        if (!operations[pass]())
            return false;
    }
    return true;
}
} // namespace kin
