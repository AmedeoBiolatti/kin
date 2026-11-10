#pragma once

#include <kin/assets/development_revision.hpp>
#include <kin/ecs/component.hpp>

#include <memory>
#include <string>

namespace kin {

struct SceneInstance;
SceneDocument authored_scene_document(EcsWorld& world, const SceneInstance& instance);
// Three-way authored patch: only fields changed between the two documents
// replace live values. Entity identities and non-reflected runtime state stay.
// Structural deletions finalize only after the surrounding transaction succeeds.
std::unique_ptr<DevelopmentChange> prepare_scene_patch2d(EcsWorld& world, const SceneDocument& before,
                                                         const SceneDocument& after, std::string& error);
} // namespace kin
