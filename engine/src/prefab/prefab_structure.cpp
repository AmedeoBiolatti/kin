#include "prefab_asset_json.hpp"
#include <kin/assets/content.hpp>
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace kin {
namespace {
void require(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
template<class M> std::vector<std::string> keys(const M& map){std::vector<std::string> out;for(const auto& [k,v]:map)out.push_back(k);std::ranges::sort(out);return out;}
std::vector<PrefabComponentAsset> component_map(const JsonValue& json,const EcsComponentRegistry& registry,std::vector<std::string>& errors){
    std::vector<PrefabComponentAsset> out;
    if(!json.is_object()){errors.push_back("prefab components must be an object");return out;}
    for(const auto& [name,value]:json.members()) {
        PrefabComponentAsset c{.name=name};for(auto f:parse_component_fields(name,value,registry,errors))c.fields.push_back({f.name,f.value});out.push_back(std::move(c));
    }return out;
}
void write_components(JsonWriter& j,const std::vector<PrefabComponentAsset>& components){
    j.begin_object();for(const auto& c:components){j.key(c.name).begin_object();for(const auto& f:c.fields){j.key(f.name);write_component_field_value_json(j,f.value);}j.end_object();}j.end_object();
}
}
namespace prefab_json {
PrefabEntityAsset entity(const JsonValue& j,const EcsComponentRegistry& r,std::vector<std::string>& e){
    if(!j.is_object()){e.push_back("prefab entity must be an object");return {};}
    PrefabEntityAsset out{.id=j.string_at("id"),.name=j.string_at("name"),.parent=j.string_at("parent")};
    if(auto* c=j.find("components"))out.components=component_map(*c,r,e);return out;
}
PrefabReferenceAsset reference(const JsonValue& j,const EcsComponentRegistry& r,std::vector<std::string>& e){
    PrefabReferenceAsset out;
    try {
        require(j.is_object(),"prefab reference must be an object");out.id=j.string_at("id");out.asset=j.string_at("asset");out.name=j.string_at("name");out.parent=j.string_at("parent");
        if(auto* p=j.find("pos")){std::string error;auto value=component_field_value_from_json(ComponentFieldKind::Vec2f,*p,error);require(bool(value),error);out.pos=std::get<Vec2f>(*value);}
        if(auto* o=j.find("overrides")){
            require(o->is_object(),"prefab overrides must be an object");for(const auto& [id,cs]:o->members())out.overrides.entities[id]=component_map(cs,r,e);
        }
        if(auto* s=j.find("structure")){
            require(s->is_object(),"prefab structure must be an object");
            for(const auto& [k,v]:s->members())require(k=="add_entities"||k=="remove_entities"||k=="remove_components"||k=="parents"||k=="names","unknown prefab structure field '"+k+"'");
            if(auto* a=s->find("add_entities")){require(a->is_array(),"add_entities must be an array");for(const auto& v:a->items())out.overrides.add_entities.push_back(entity(v,r,e));}
            if(auto* a=s->find("remove_entities")){require(a->is_array(),"remove_entities must be an array");for(const auto& v:a->items()){require(v.is_string(),"remove_entities needs string IDs");out.overrides.remove_entities.push_back(v.as_string());}}
            if(auto* a=s->find("remove_components")){require(a->is_object(),"remove_components must be an object");for(const auto& [id,v]:a->members()){require(v.is_array(),"remove_components needs arrays");for(const auto& c:v.items()){require(c.is_string(),"component names must be strings");out.overrides.remove_components[id].push_back(c.as_string());}}}
            for(auto field:{"parents","names"})if(auto* a=s->find(field)){require(a->is_object(),"parents/names must be objects");for(const auto& [id,v]:a->members()){require(v.is_string(),"parents/names values must be strings");(std::string_view(field)=="parents"?out.overrides.parents:out.overrides.names)[id]=v.as_string();}}
        }
    }catch(const std::exception& error){e.push_back(error.what());}
    return out;
}
void write_entity(JsonWriter& j,const PrefabEntityAsset& e){j.begin_object();j.field("id",std::string_view(e.id));if(!e.name.empty())j.field("name",std::string_view(e.name));if(!e.parent.empty())j.field("parent",std::string_view(e.parent));j.key("components");write_components(j,e.components);j.end_object();}
void write_overrides(JsonWriter& j,const PrefabOverrideSet& o){
    if(!o.entities.empty()){j.key("overrides").begin_object();for(const auto& id:keys(o.entities)){j.key(id);write_components(j,o.entities.at(id));}j.end_object();}
    if(!o.structural())return;j.key("structure").begin_object();
    if(!o.add_entities.empty()){j.key("add_entities").begin_array();for(const auto& e:o.add_entities)write_entity(j,e);j.end_array();}
    if(!o.remove_entities.empty()){j.key("remove_entities").begin_array();auto ids=o.remove_entities;std::ranges::sort(ids);for(const auto& id:ids)j.value(std::string_view(id));j.end_array();}
    if(!o.remove_components.empty()){j.key("remove_components").begin_object();for(const auto& id:keys(o.remove_components)){j.key(id).begin_array();auto cs=o.remove_components.at(id);std::ranges::sort(cs);for(const auto& c:cs)j.value(std::string_view(c));j.end_array();}j.end_object();}
    for(auto field:{"parents","names"}){const auto& values=std::string_view(field)=="parents"?o.parents:o.names;if(values.empty())continue;j.key(field).begin_object();for(const auto& id:keys(values))j.field(id,std::string_view(values.at(id)));j.end_object();}
    j.end_object();
}
void write_reference(JsonWriter& j,const PrefabReferenceAsset& p){j.begin_object();j.field("id",std::string_view(p.id));j.field("asset",std::string_view(p.asset.string()));if(!p.name.empty())j.field("name",std::string_view(p.name));if(!p.parent.empty())j.field("parent",std::string_view(p.parent));if(p.pos!=Vec2f{}){j.key("pos");write_component_field_value_json(j,p.pos);}write_overrides(j,p.overrides);j.end_object();}
}

const PrefabAsset& resolved_prefab_asset(const PrefabAsset& p){return p.expanded?*p.expanded:p;}
PrefabBuildResult apply_prefab_overrides(const PrefabAsset& source,const PrefabOverrideSet& o,const EcsComponentRegistry& registry,bool include_fields){
    PrefabBuildResult result;
    try{
        auto p=resolved_prefab_asset(source);p.expanded.reset();p.source_files.clear();
        require(p.prefabs.empty(),"resolve prefab composition before applying overrides");
        std::unordered_map<std::string,PrefabEntityAsset*> entities;
        std::unordered_set<std::string> ids;for(const auto& e:p.entities)ids.insert(e.id);
        for(const auto& e:o.add_entities){require(!e.id.empty()&&ids.insert(e.id).second,"added entity ID duplicates source or another addition: '"+e.id+"'");require(!e.parent.empty(),"added prefab children require a parent inside the instance");}
        p.entities.insert(p.entities.end(),o.add_entities.begin(),o.add_entities.end());for(auto& e:p.entities)entities[e.id]=&e;
        const auto get=[&](const std::string& id)->PrefabEntityAsset&{require(entities.contains(id),"structural override references unknown entity '"+id+"'");return *entities.at(id);};
        for(const auto& [id,parent]:o.parents){require(id!=p.root,"cannot reparent prefab root");require(!parent.empty(),"child parent must remain inside the prefab instance");get(id).parent=parent;}
        for(const auto& [id,name]:o.names){require(!name.empty(),"override names cannot be empty");get(id).name=name;}
        std::unordered_set<std::string> removed;
        for(const auto& id:o.remove_entities){get(id);require(id!=p.root,"cannot remove prefab root");require(removed.insert(id).second,"duplicate remove_entities ID");}
        bool changed=true;while(changed){changed=false;for(const auto& e:p.entities)if(removed.contains(e.parent)&&removed.insert(e.id).second)changed=true;}
        require(!removed.contains(p.root),"cannot remove root through a parent cycle");
        for(const auto& [id,name]:o.names)require(!removed.contains(id),"name override targets removed entity");
        for(const auto& [id,parent]:o.parents)require(!removed.contains(id),"parent override targets removed entity");
        for(const auto& [id,cs]:o.remove_components){auto& e=get(id);require(!removed.contains(id),"component removal targets removed entity");std::unordered_set<std::string> seen;
            for(const auto& c:cs){require(seen.insert(c).second,"duplicate component removal");const auto before=e.components.size();std::erase_if(e.components,[&](const auto& v){return v.name==c;});require(before!=e.components.size(),"cannot remove absent component '"+c+"' from '"+id+"'");}}
        for(const auto& [id,cs]:o.entities){auto& e=get(id);require(!removed.contains(id),"field override targets removed entity");
            // Validate patch fields independently of whether they are compiled.
            PrefabAsset patch{.id="patch",.root=id,.entities={{.id=id,.components=cs}}};auto errors=validate_prefab_asset(patch,registry);require(errors.empty(),errors.empty()?"":errors.front());
            if(include_fields)for(const auto& c:cs){auto found=std::ranges::find(e.components,c.name,&PrefabComponentAsset::name);if(found==e.components.end())e.components.push_back(c);else for(const auto& f:c.fields){auto field=std::ranges::find(found->fields,f.name,&PrefabFieldAsset::name);if(field==found->fields.end())found->fields.push_back(f);else *field=f;}}
        }
        std::erase_if(p.entities,[&](const auto& e){return removed.contains(e.id);});
        if(include_fields)for(const auto& e:p.entities)for(const auto& c:e.components)for(const auto& f:c.fields)if(const auto* ref=std::get_if<ComponentEntityRef>(&f.value))require(!removed.contains(ref->authored_id),"reference targets removed prefab entity '"+ref->authored_id+"'");
        auto errors=validate_prefab_asset(p,registry);require(errors.empty(),errors.empty()?"":errors.front());result.prefab=std::move(p);
    }catch(const std::exception& error){result.diagnostics.push_back(error.what());}return result;
}

PrefabLoadResult load_prefab_asset(const std::filesystem::path& path,const EcsComponentRegistry& registry,PrefabAssetLoadOptions options){
    struct Loader {
        const EcsComponentRegistry& registry;PrefabAssetLoadOptions options;
        std::unordered_map<std::string,PrefabAsset> cache;std::unordered_set<std::string> active;
        PrefabAsset load(std::filesystem::path file,size_t depth){
            file=std::filesystem::absolute(file).lexically_normal();const auto key=file.string();
            require(depth<options.max_depth,"prefab composition exceeds depth limit");require(!active.contains(key),"prefab composition cycle at '"+key+"'");
            if(auto found=cache.find(key);found!=cache.end())return found->second;
            require(cache.size()+active.size()<options.max_sources,"prefab composition exceeds source limit");
            const auto size=content_file_size(file);require(size&&(!options.max_source_bytes||*size<=options.max_source_bytes),"prefab source missing or exceeds byte limit: "+key);
            auto text=read_content_text(file);require(bool(text),"cannot read prefab: "+key);auto parsed=parse_prefab_asset(*text,registry,key);require(parsed.ok(),parsed.diagnostics.empty()?"invalid prefab":parsed.diagnostics.front());
            auto p=std::move(*parsed.prefab);active.insert(key);PrefabAsset flat=p;flat.prefabs.clear();p.source_files={file};
            require(!options.max_entities||flat.entities.size()<=options.max_entities,"prefab exceeds expanded entity limit");
            for(const auto& ref:p.prefabs){auto child=load(options.content_root/ref.asset,depth+1);auto modified=apply_prefab_overrides(child,ref.overrides,registry);require(modified.ok(),modified.diagnostics.empty()?"invalid composed override":modified.diagnostics.front());
                const auto& nested=*modified.prefab;
                const auto map=[&](const std::string& id){return id==nested.root?ref.id:ref.id+"/"+id;};
                std::unordered_set<std::string> locals;for(const auto& e:nested.entities)locals.insert(e.id);
                require(!options.max_entities||nested.entities.size()<=options.max_entities-flat.entities.size(),"prefab exceeds expanded entity limit");
                for(auto e:nested.entities){const bool root=e.id==nested.root;e.id=map(e.id);e.parent=root?(ref.parent.empty()?p.root:ref.parent):e.parent.empty()?ref.id:map(e.parent);
                    if(root&&!ref.name.empty())e.name=ref.name;
                    if(root&&ref.pos!=Vec2f{}){auto c=std::ranges::find(e.components,std::string("Transform2D"),&PrefabComponentAsset::name);if(c==e.components.end()){e.components.push_back({.name="Transform2D"});c=std::prev(e.components.end());}auto f=std::ranges::find(c->fields,std::string("pos"),&PrefabFieldAsset::name);if(f==c->fields.end())c->fields.push_back({"pos",ref.pos});else f->value=ref.pos;}
                    for(auto& c:e.components)for(auto& f:c.fields)if(auto* value=std::get_if<ComponentEntityRef>(&f.value);value&&locals.contains(value->authored_id)){value->authored_id=map(value->authored_id);value->id=0;}
                    flat.entities.push_back(std::move(e));
                }
                p.source_files.insert(p.source_files.end(),child.source_files.begin(),child.source_files.end());
            }
            require(!options.max_entities||flat.entities.size()<=options.max_entities,"prefab exceeds expanded entity limit");
            auto errors=validate_prefab_asset(flat,registry);require(errors.empty(),errors.empty()?"":errors.front());
            if(!p.prefabs.empty())p.expanded=std::make_shared<const PrefabAsset>(std::move(flat));
            std::ranges::sort(p.source_files);p.source_files.erase(std::unique(p.source_files.begin(),p.source_files.end()),p.source_files.end());active.erase(key);cache.emplace(key,p);return p;
        }
    };
    try {if(options.content_root.empty())options.content_root=std::filesystem::absolute(path).parent_path();Loader loader{registry,std::move(options)};return {.prefab=loader.load(path,0)};}
    catch(const std::exception& error){return {.diagnostics={error.what()}};}
}
}
