#include <cassert>
#include <iostream>
#include <kin/renderer/resource_graph.hpp>
using namespace kin;
int main() {
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
    std::cout << "resource graph: validation, stable scheduling, cache invalidation, culling and lifetime slots\n";
}
