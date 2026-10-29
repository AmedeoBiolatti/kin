#include <cassert>
#include <iostream>
#include <kin/renderer/resource_graph.hpp>
using namespace kin;
namespace {
ResourceGraphDesc versions() {
    ResourceGraphDesc g;
    for (auto name : {"opaque", "transparent", "final"})
        g.resources.push_back({.name = name,
                               .kind = GraphResourceKind::RenderAttachment,
                               .format = TextureFormat::Rgba16Float,
                               .size = {64, 32}});
    g.resources.back().exported = true;
    g.passes = {
        {.name = "opaque", .attachments = {{.resource = 0}}},
        {.name = "transparent", .attachments = {{.resource = 1, .previous = 0, .load = GraphAttachmentLoad::Load}}},
        {.name = "final", .attachments = {{.resource = 2, .previous = 1, .load = GraphAttachmentLoad::Load}}}};
    return g;
}
constexpr auto attachment_features = graph_feature::attachments | graph_feature::texture_copies;
std::vector<u64> simulate(const CompiledResourceGraph& p) {
    assert(p);
    std::vector<u64> slots(p.slots.size()), observed;
    std::vector<std::function<bool()>> operations;
    for (u32 i = 0; i < p.source.passes.size(); ++i)
        operations.push_back([&, i] {
            const auto& pass = p.source.passes[i];
            u64 sampled = 0;
            for (auto r : pass.reads) {
                const auto v = slots[p.lifetimes[r].slot];
                observed.push_back(v);
                sampled = sampled * 17 + v;
            }
            for (const auto& a : pass.attachments) {
                auto& value = slots[p.lifetimes[a.resource].slot];
                if (a.access == GraphAttachmentAccess::ReadOnlyDepth) {
                    observed.push_back(value);
                    continue;
                }
                if (a.load != GraphAttachmentLoad::Load)
                    value = 7;
                value = value * 31 + i + sampled;
                observed.push_back(value);
            }
            return true;
        });
    assert(execute_resource_graph(p, operations, [&](const GraphAttachmentCopy& c) {
        assert(c.bytes == graph_resource_payload(p.source.resources[c.to]));
        slots[p.lifetimes[c.to].slot] = slots[p.lifetimes[c.from].slot];
        return true;
    }));
    return observed;
}
void attachment_versions() {
    ResourceGraphCompiler c;
    auto g = versions();
    const auto fast = c.compile(g, graph_feature::attachments);
    assert(fast && fast.slots.size() == 1 && fast.attachment_reuses == 2 && fast.attachment_copies.empty());
    assert(fast.slot_payload_bytes == 64 * 32 * 8);
    const auto slow = c.compile(g, attachment_features, {.reuse_slots = false});
    assert(slow && slow.slots.size() == 3 && slow.attachment_copies.size() == 2 &&
           slow.attachment_copy_bytes == 2 * 64 * 32 * 8);
    assert(simulate(fast) == simulate(slow));
    std::vector<std::function<bool()>> ops(g.passes.size(), [] {
        assert(false);
        return true;
    });
    assert(!execute_resource_graph(slow, ops)); // no copy recorder: no operation runs
    ops[2] = {};
    assert(!execute_resource_graph(slow, ops, [](const auto&) {
        assert(false);
        return true;
    }));
    assert(c.compile(g, attachment_features, {.max_slot_payload_bytes = fast.slot_payload_bytes}));
    assert(
        !c.compile(g, attachment_features, {.reuse_slots = false, .max_slot_payload_bytes = fast.slot_payload_bytes}));
    // A later reader needs the old value. A culled reader does not.
    g.passes.push_back({.name = "inspect", .reads = {0}, .after = {"final"}, .side_effect = true});
    auto branch = c.compile(g, attachment_features);
    assert(branch && branch.attachment_copies.size() == 1 &&
           simulate(branch) == simulate(c.compile(g, attachment_features, {.reuse_slots = false})));
    assert(!c.compile(g, graph_feature::attachments));
    g.passes.back().side_effect = false;
    assert(c.compile(g, graph_feature::attachments).attachment_copies.empty());
    g = versions();
    g.passes[1].reads = {0}; // sampling the previous image forces distinct storage even at last use
    auto sampled = c.compile(g, attachment_features);
    assert(sampled && sampled.attachment_copies.size() == 1 &&
           simulate(sampled) == simulate(c.compile(g, attachment_features, {.reuse_slots = false})));
    for (int pin = 0; pin < 3; ++pin) {
        g = versions();
        g.resources[0].exported = pin == 0;
        g.resources[0].persistent = pin == 1;
        g.resources[1].persistent = pin == 2;
        const auto p = c.compile(g, attachment_features);
        assert(p && !p.attachment_copies.empty() &&
               simulate(p) == simulate(c.compile(g, attachment_features, {.reuse_slots = false})));
    }
    g = versions();
    g.resources[0].imported = true;
    g.passes.erase(g.passes.begin());
    const auto imported = c.compile(g, attachment_features);
    assert(imported && imported.attachment_copies.size() == 1 && imported.lifetimes[0].slot == graph_no_slot);
    // Two loads of the same old version in one pass: at most one can own its slot.
    g = versions();
    g.passes.resize(2);
    g.passes[1].attachments.push_back({.resource = 2, .previous = 0, .load = GraphAttachmentLoad::Load});
    const auto fork = c.compile(g, attachment_features);
    assert(fork && fork.attachment_reuses == 1 && fork.attachment_copies.size() == 1 &&
           fork.lifetimes[1].slot != fork.lifetimes[2].slot &&
           simulate(fork) == simulate(c.compile(g, attachment_features, {.reuse_slots = false})));
    auto invalid = [&](auto change) {
        auto bad = versions();
        change(bad);
        const auto& p = c.compile(bad, ~u64{0});
        assert(!p && !p.diagnostics.empty());
    };
    invalid([](auto& x) { x.passes[1].attachments[0].previous = 1; });
    invalid([](auto& x) { x.passes[1].attachments[0].previous = graph_no_slot; });
    invalid([](auto& x) { x.passes[1].attachments[0].load = GraphAttachmentLoad::Clear; });
    invalid([](auto& x) { x.passes[0].attachments[0].load = GraphAttachmentLoad(99); });
    invalid([](auto& x) { x.passes[0].attachments[0].store = GraphAttachmentStore(99); });
    invalid([](auto& x) { x.passes[0].attachments[0].access = GraphAttachmentAccess(99); });
    invalid([](auto& x) { x.passes[0].attachments[0].resource = 99; });
    invalid([](auto& x) { x.resources[1].size.x *= 2; });
    invalid([](auto& x) { x.resources[1].array_layers = 1; });
    invalid([](auto& x) { x.resources[1].format = TextureFormat::Rg16Float; });
    invalid([](auto& x) { x.resources[1].kind = GraphResourceKind::StorageTexture; });
    invalid([](auto& x) { x.passes[0].writes = {0}; });
    invalid([](auto& x) { x.passes[0].reads = {0}; });
    invalid([](auto& x) { x.passes[0].attachments[0].store = GraphAttachmentStore::Discard; });
    invalid([](auto& x) { x.passes[2].attachments[0].store = GraphAttachmentStore::Discard; });
    invalid([](auto& x) { x.passes[0].attachments[0].access = GraphAttachmentAccess::ReadOnlyDepth; });
    g = versions();
    g.resources.push_back({.name = "depth",
                           .kind = GraphResourceKind::RenderAttachment,
                           .format = TextureFormat::Depth32Float,
                           .size = {64, 32}});
    g.passes[0].attachments.push_back({.resource = 3});
    g.passes[1].attachments.push_back(
        {.resource = 3, .load = GraphAttachmentLoad::Load, .access = GraphAttachmentAccess::ReadOnlyDepth});
    assert(c.compile(g, graph_feature::attachments));
    g.passes[1].reads = {3};
    assert(!c.compile(g, attachment_features)); // depth test attachment is not a shader sample
    g.passes[1].reads.clear();
    g.passes[1].attachments.back().load = GraphAttachmentLoad::Clear;
    assert(!c.compile(g, attachment_features));
    // Load/store and access are part of structural cache identity.
    g = versions();
    const auto compiled = c.stats().compilations;
    assert(c.compile(g, attachment_features));
    assert(c.compile(g, attachment_features));
    assert(c.stats().compilations == compiled + 1);
    g.passes[0].attachments[0].load = GraphAttachmentLoad::Discard;
    assert(c.compile(g, attachment_features) && c.stats().compilations == compiled + 2);
    g = versions();
    for (auto& r : g.resources)
        r.size = {1 << 30, 1 << 30};
    assert(!c.compile(g, attachment_features)); // logical live-byte arithmetic cannot wrap after a handoff
    // Reproducible branching graphs compare all observed contents with unaliased execution.
    u32 rng = 0x213a7;
    auto random = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    };
    for (u32 trial = 0; trial < 300; ++trial) {
        g = {};
        for (u32 i = 0; i < 30; ++i) {
            g.resources.push_back({.name = std::to_string(i),
                                   .kind = GraphResourceKind::RenderAttachment,
                                   .format = TextureFormat::Rgba16Float,
                                   .size = {8, 8},
                                   .exported = random() % 7 == 0});
            GraphPassDesc pass{.name = std::to_string(i), .side_effect = true};
            GraphAttachment a{.resource = i};
            if (i) {
                a.previous = random() % i;
                a.load = GraphAttachmentLoad::Load;
                if (random() % 4 == 0)
                    pass.reads.push_back(a.previous);
                if (random() % 3 == 0 && (pass.reads.empty() || pass.reads[0] != 0))
                    pass.reads.push_back(0);
            }
            pass.attachments.push_back(a);
            g.passes.push_back(std::move(pass));
        }
        const auto optimized = c.compile(g, attachment_features);
        const auto reference = c.compile(g, attachment_features, {.reuse_slots = false});
        assert(optimized && reference && simulate(optimized) == simulate(reference));
    }
    std::cout << "attachments: version handoffs, branches, pinned/imported/sample preservation, depth access, failure "
                 "admission and 300 reference graphs\n";
}
} // namespace

int main() {
    attachment_versions();
    ResourceGraphDesc graph;
    for (auto name : {"a", "b", "c", "output", "unused"})
        graph.resources.push_back({.name = name, .kind = GraphResourceKind::ColorTarget, .size = {64, 32}});
    graph.resources[3].exported = true;
    graph.passes = {{.name = "finish", .reads = {2}, .writes = {3}},
                    {.name = "start", .writes = {0}},
                    {.name = "middle", .reads = {0}, .writes = {1}},
                    {.name = "ascii", .reads = {1}, .writes = {2}, .required_features = graph_feature::shaders},
                    {.name = "unused", .writes = {4}, .required_features = graph_feature::compute}};
    ResourceGraphCompiler compiler;
    const auto features = graph_feature::raster | graph_feature::shaders;
    auto plan = compiler.compile(graph, features);
    assert(plan && plan.order == (std::vector<u32>{1, 2, 3, 0}));
    assert(plan.culled_passes == 1 && plan.slots.size() == 3);
    assert(plan.lifetimes[0].slot == plan.lifetimes[2].slot);
    assert(plan.lifetimes[0].slot != plan.lifetimes[1].slot);
    assert(plan.slot_payload_bytes == 3 * 64 * 32 * 4 && plan.peak_live_payload_bytes == 2 * 64 * 32 * 4);
    assert(compiler.compile(graph, features));
    assert(compiler.stats().compilations == 1 && compiler.stats().cache_hits == 1);
    std::vector<u32> executed;
    std::vector<std::function<bool()>> operations;
    for (u32 i = 0; i < graph.passes.size(); ++i)
        operations.push_back([&, i] {
            executed.push_back(i);
            return true;
        });
    assert(execute_resource_graph(plan, operations) && executed == plan.order);
    operations[0] = {};
    executed.clear();
    assert(!execute_resource_graph(plan, operations) && executed.empty());
    auto reference = compiler.compile(graph, ~u64{0}, {.cull_unused = false, .reuse_slots = false});
    assert(reference && reference.slots.size() == 5);
    graph.resources[3].size = {128, 32};
    assert(compiler.compile(graph, features) && compiler.stats().compilations == 3);
    const auto invalid = [&](auto change) {
        auto broken = graph;
        change(broken);
        const auto& rejected = compiler.compile(broken, features);
        assert(!rejected && !rejected.diagnostics.empty());
        executed.clear();
        assert(!execute_resource_graph(rejected, operations) && executed.empty());
    };
    invalid([](auto& g) { g.passes[1].reads = {3}; }); // cycle
    invalid([](auto& g) { g.passes[1].after = {"absent"}; });
    invalid([](auto& g) { g.passes[1].writes = {0, 1}; }); // two writers
    invalid([](auto& g) { g.passes[2].writes = {0}; });    // same-pass feedback
    invalid([](auto& g) { g.passes[1].writes = {99}; });
    invalid([](auto& g) { g.passes[1].writes.clear(); });
    invalid([](auto& g) { g.passes[1].enabled = false; });
    invalid([](auto& g) { g.passes[0].required_features = graph_feature::compute; });
    invalid([](auto& g) { g.resources[0].imported = true; });
    invalid([](auto& g) { g.resources[1].size.x = 0; });
    invalid([](auto& g) { g.passes[1].name = "finish"; });
    invalid([](auto& g) { g.resources[1].name = "a"; });
    const auto& over_budget = compiler.compile(graph, features, {.max_slot_payload_bytes = 1});
    assert(!over_budget && !execute_resource_graph(over_budget, operations));
    // Imported history is read-only; this frame's history is a separate output.
    graph.resources[0].imported = true;
    graph.passes[1].writes.clear();
    assert(compiler.compile(graph, features));
    ResourceGraphDesc passthrough{.resources = {{.name = "external-output",
                                                 .kind = GraphResourceKind::ColorTarget,
                                                 .size = {8, 8},
                                                 .imported = true,
                                                 .exported = true}}};
    const auto& imported = compiler.compile(passthrough, 0);
    assert(imported && imported.order.empty() && imported.lifetimes[0].first == 0 && imported.slots.empty());
    GraphResourceDesc hdr{.name = "hdr", .kind = GraphResourceKind::ColorTarget, .size = {64, 32}, .hdr = true};
    assert(graph_resource_payload(hdr) == 64 * 32 * 8);
    // Explicit data attachments have exact payloads, separate compatible slots,
    // capability admission and no working-colour/HDR interpretation.
    ResourceGraphDesc geometry;
    geometry.resources = {{.name = "depth",
                           .kind = GraphResourceKind::RenderAttachment,
                           .format = TextureFormat::Depth32Float,
                           .size = {64, 32}},
                          {.name = "normal",
                           .kind = GraphResourceKind::RenderAttachment,
                           .format = TextureFormat::Rg16Float,
                           .size = {64, 32}},
                          {.name = "motion",
                           .kind = GraphResourceKind::RenderAttachment,
                           .format = TextureFormat::Rgba16Float,
                           .size = {64, 32},
                           .exported = true}};
    geometry.passes = {{.name = "geometry", .writes = {0, 1}}, {.name = "motion", .reads = {0, 1}, .writes = {2}}};
    const auto typed = compiler.compile(geometry, graph_feature::attachments);
    assert(typed && typed.slots.size() == 3 && typed.slot_payload_bytes == 64 * 32 * 16);
    assert(typed.peak_live_payload_bytes == typed.slot_payload_bytes);
    assert(!compiler.compile(geometry, 0));
    auto oversized = geometry;
    oversized.resources[2].size = {0x7fffffff, 0x7fffffff};
    assert(texture_level_bytes(TextureFormat::Rgba16Float, oversized.resources[2].size) == ~u64{0});
    assert(!compiler.compile(oversized, graph_feature::attachments));
    geometry.resources[1].hdr = true;
    assert(!compiler.compile(geometry, graph_feature::attachments));
    geometry.resources[1].hdr = false;
    geometry.resources[1].format = TextureFormat::Rgba8;
    assert(!compiler.compile(geometry, graph_feature::attachments));
    geometry.resources[1].format = TextureFormat::Rg16Float;
    geometry.resources[0].array_layers = 4;
    constexpr auto array_features = graph_feature::attachments | graph_feature::attachment_arrays;
    const auto arrays = compiler.compile(geometry, array_features);
    assert(arrays && arrays.slot_payload_bytes == 64 * 32 * 28);
    assert(graph_resource_payload(geometry.resources[0]) == 64 * 32 * 4 * 4);
    assert(!compiler.compile(geometry, graph_feature::attachments));
    auto wrong_kind = geometry;
    wrong_kind.resources[0].kind = GraphResourceKind::StorageTexture;
    assert(!compiler.compile(wrong_kind, ~u64{0}));
    auto overflow_array = geometry;
    overflow_array.resources[0].size = {0x7fffffff, 0x7fffffff};
    overflow_array.resources[0].array_layers = 4;
    assert(!compiler.compile(overflow_array, array_features));
    // One-layer arrays and 2D textures cannot share physical slots.
    ResourceGraphDesc array_slots;
    array_slots.resources = {{.name = "2d",
                              .kind = GraphResourceKind::RenderAttachment,
                              .format = TextureFormat::Depth32Float,
                              .size = {8, 8}},
                             {.name = "array",
                              .kind = GraphResourceKind::RenderAttachment,
                              .format = TextureFormat::Depth32Float,
                              .size = {8, 8},
                              .exported = true,
                              .array_layers = 1}};
    array_slots.passes = {{.name = "2d", .writes = {0}, .side_effect = true},
                          {.name = "array", .writes = {1}, .after = {"2d"}}};
    const auto distinct = compiler.compile(array_slots, array_features);
    assert(distinct && distinct.slots.size() == 2);
    assert(texture_format_compressed(TextureFormat::Bc1RgbaUnorm));
    assert(texture_format_compressed(TextureFormat::Astc4x4RgbaSrgb));
    for (auto format : {TextureFormat::Rg16Float, TextureFormat::Rgba16Float, TextureFormat::Depth32Float,
                        TextureFormat::Depth24Unorm})
        assert(!texture_format_compressed(format));
    std::cout << "resource graph: validation, stable scheduling, cache invalidation, culling and lifetime slots\n";
}
