#pragma once
#include <kin/prefab/prefab_asset.hpp>
#include <kin/core/json_value.hpp>
#include <kin/core/json.hpp>
namespace kin::prefab_json {
PrefabEntityAsset entity(const JsonValue&,const EcsComponentRegistry&,std::vector<std::string>&);
PrefabReferenceAsset reference(const JsonValue&,const EcsComponentRegistry&,std::vector<std::string>&);
void write_entity(JsonWriter&,const PrefabEntityAsset&);
void write_reference(JsonWriter&,const PrefabReferenceAsset&);
void write_overrides(JsonWriter&,const PrefabOverrideSet&);
}
