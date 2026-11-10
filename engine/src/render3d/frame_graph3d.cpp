#include <kin/render3d/frame_graph3d.hpp>
#include <kin/renderer/resource_graph.hpp>

namespace kin {

const ResourceGraphDesc& stock_frame_graph3d() {
    // Coarse stock operations use the public compiler. Tokens cover backend-owned
    // resources; their existing allocation/history ownership stays explicit. Inner
    // prepass/main subpasses remain atomic operations in this first migration.
    static const ResourceGraphDesc recipe = [] {
        ResourceGraphDesc graph;
        for (const auto* name :
             {"geometry", "clusters", "local_shadows", "gi_attributes", "gi_probes", "sun_shadow", "prepass",
              "occlusion", "overlay_velocity", "colour", "fog_colour", "resolved", "pick", "output"})
            graph.resources.push_back({.name = name});
        graph.resources[0].imported = true;
        graph.resources[13].exported = true;
        graph.passes = {{.name = "clusters", .reads = {0}, .writes = {1}},
                        {.name = "local_shadows", .reads = {0, 1}, .writes = {2}},
                        {.name = "gi_scene", .reads = {0, 2}, .writes = {3}},
                        {.name = "gi_probes", .reads = {3}, .writes = {4}},
                        {.name = "sun_shadow", .reads = {0, 1}, .writes = {5}},
                        {.name = "prepass", .reads = {1, 4, 5}, .writes = {6}},
                        {.name = "occluders", .reads = {6}, .writes = {7}},
                        {.name = "overlay_velocity", .reads = {6, 7}, .writes = {8}},
                        {.name = "main", .reads = {1, 2, 4, 5, 6, 7, 8}, .writes = {9}},
                        {.name = "fog_sun", .reads = {9, 5}, .writes = {10}},
                        {.name = "temporal_resolve", .reads = {10, 8}, .writes = {11}},
                        {.name = "picking", .reads = {0, 1}, .writes = {12}, .after = {"temporal_resolve"}},
                        {.name = "output", .reads = {11, 12}, .writes = {13}}};
        return graph;
    }();
    return recipe;
}

} // namespace kin
