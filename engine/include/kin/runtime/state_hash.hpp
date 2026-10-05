#pragma once

#include <kin/core/types.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace kin {

class SceneManager;

// One piece of a frame's game state, for finding where two runs differ.
struct StateItem {
    i32 scene = 0;
    std::string scene_name;
    u64 entity = 0; // 0 for the scene's report
    std::string entity_name;
    // The component (or pair) name; "(type)" for the entity's set of components
    // and tags; "(report)" for the scene's write_report state.
    std::string component;
    u64 hash = 0;
    std::vector<u8> bytes; // the value: component bytes, or the report's JSON
};

struct StateCoverage {
    i32 scenes = 0; // each compared by its write_report
    i32 entities = 0;
    i32 values = 0; // component values hashed
    // Component types not compared: ones that own resources (strings,
    // containers, handles), whose bytes hold addresses that differ between runs.
    std::vector<std::string> not_compared;
};

// A hash of the scenes' state: in each scene's ECS world, every entity with its
// components and tags and the bytes of its plain-data components (trivially
// copyable ones), and each scene's write_report. The same state hashes the
// same in any process.
u64 hash_state(SceneManager& scenes, StateCoverage* coverage = nullptr);

// The same state, item by item, in a stable order.
std::vector<StateItem> describe_state(SceneManager& scenes);

u64 hash_bytes(const void* data, std::size_t size, u64 seed = 0xcbf29ce484222325ull);

} // namespace kin
