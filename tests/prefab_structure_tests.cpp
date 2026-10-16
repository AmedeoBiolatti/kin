#include <kin/scene/scene_asset_runtime.hpp>
#include <kin/ecs/render.hpp>
#include <kin/assets/content.hpp>
#include <kin/assets/asset_manager.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace kin;
struct NativeState {u32 value=0;std::shared_ptr<int> runtime;};
void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
struct Scratch{std::filesystem::path path=std::filesystem::temp_directory_path()/("kin-composition-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));Scratch(){std::filesystem::create_directories(path);}~Scratch(){std::error_code e;std::filesystem::remove_all(path,e);}};
void write(const std::filesystem::path& p,std::string_view text){std::ofstream(p)<<text;static int revision=0;std::filesystem::last_write_time(p,std::filesystem::file_time_type::clock::now()+std::chrono::seconds(++revision));}
void configure(kin::EcsWorld& w){register_builtin_editor_components(w);w.components().data("Target").field_entity_ref("other");w.components().data("Tag");w.components().native<NativeState>("NativeState").field("value",&NativeState::value);}
constexpr auto inner=R"({"schema":"kin.prefab/1","id":"inner","root":"body","entities":[
 {"id":"body","components":{"Transform2D":{"pos":[1,2]},"RectRenderer":{"size":[2,3],"visible":false},"Timer":{"duration":10},"Tag":{},"NativeState":{"value":9}}},
 {"id":"spark","parent":"body","components":{"Transform2D":{},"Target":{"other":"body"}}},
 {"id":"ghost","parent":"spark","components":{"Transform2D":{}}}]})";
constexpr auto outer=R"({"schema":"kin.prefab/1","id":"outer","root":"root","entities":[
 {"id":"root","components":{"Transform2D":{},"Target":{"other":"gadget/spark"}}},
 {"id":"attachment","parent":"gadget/spark","components":{"Transform2D":{}}}],"prefabs":[
 {"id":"gadget","asset":"inner.kinprefab","parent":"root","overrides":{"body":{"RectRenderer":{"size":[4,5]}}},
  "structure":{"remove_entities":["ghost"],"add_entities":[{"id":"bell","parent":"body","components":{"Timer":{"duration":3}}}]}}]})";
constexpr auto scene=R"({"schema":"kin.scene/1","prefabs":[
 {"id":"one","asset":"outer.kinprefab","overrides":{"root":{"Target":{"other":"added"}},"gadget":{"RectRenderer":{"size":[10,11]}}},
 "structure":{"remove_entities":["gadget/spark"],"remove_components":{"gadget":["Timer","RectRenderer"]},"parents":{"gadget/bell":"root"},"names":{"gadget":"Custom gadget"},
  "add_entities":[{"id":"added","parent":"gadget","components":{"Transform2D":{},"Timer":{"duration":5}}}]}},
 {"id":"two","asset":"outer.kinprefab"}]})";
void composition_and_roundtrip(){
    Scratch scratch;write(scratch.path/"inner.kinprefab",inner);write(scratch.path/"outer.kinprefab",outer);write(scratch.path/"scene.kinscene",scene);
    kin::EcsWorld world;configure(world);auto loaded=load_prefab_asset(scratch.path/"outer.kinprefab",world.components());
    check(loaded.ok(),loaded.diagnostics.empty()?"load failed":loaded.diagnostics.front());
    check(loaded.prefab->prefabs.size()==1&&resolved_prefab_asset(*loaded.prefab).entities.size()==5&&loaded.prefab->source_files.size()==2,"composition expansion/link/dependencies missing");
    std::ostringstream raw;write_prefab_asset(raw,*loaded.prefab);auto parsed=parse_prefab_asset(raw.str(),world.components());
    check(parsed.ok()&&parsed.prefab->prefabs.size()==1&&parsed.prefab->entities.size()==2,"writing composed asset unpacked links");
    check(!instantiate_prefab(world,*parsed.prefab,{},world.components()).ok(),"unresolved composition silently instantiated");
    SceneAssetRuntime runtime;check(runtime.load(scratch.path/"scene.kinscene",scratch.path,world.components()),"scene validation failed");
    check(runtime.entity_count()==9&&runtime.prefab_sources().size()>=2,"expanded entity budget count wrong");
    check(runtime.instantiate(world),runtime.diagnostics().empty()?"instantiate failed":runtime.diagnostics().front());
    auto one=runtime.instance().by_authored_id.at("one.gadget"),two=runtime.instance().by_authored_id.at("two.gadget");
    check(one.get<RectRenderer>()->size==Vec2f{10,11}&&one.get<RectRenderer>()->visible&&!world.components().has(one,"Timer"),"remove/replace component failed");
    check(two.get<RectRenderer>()->size==Vec2f{4,5}&&!two.get<RectRenderer>()->visible&&world.components().has(two,"Timer"),"structural variant mutated source/other instance");
    check(one.name()=="Custom gadget"&&!runtime.instance().by_authored_id.contains("one.gadget/spark")&&!runtime.instance().by_authored_id.contains("one.attachment"),"rename/subtree removal failed");
    auto root=runtime.instance().by_authored_id.at("one"),added=runtime.instance().by_authored_id.at("one.added");
    auto ref=world.components().snapshot(root,"Target");check(ref&&std::get<ComponentEntityRef>(ref->fields[0].value).id==added.id(),"added entity ref not resolved");
    check(runtime.instance().by_authored_id.at("one.gadget/bell").parent().id()==root.id(),"reparent failed");
    auto extra=world.entities().create({.name="extra"}).set(Transform2D{}).child_of(root);
    check(link_prefab_entity(world,root,extra,"extra"),"new child adoption failed");
    check(!link_prefab_entity(world,root,added,"extra"),"duplicate child ID accepted");
    const auto resource=std::make_shared<int>(42);two.get_mut<NativeState>()->runtime=resource;
    check(remove_prefab_component(world,two,"Timer")&&!world.components().has(two,"Timer")&&two.get<RectRenderer>()->size==Vec2f{4,5},"editor removal exposed inherited component or lost native state");
    check(two.get<NativeState>()->runtime==resource&&two.get<NativeState>()->value==9&&world.components().has(two,"Tag"),"detaching inheritance lost native runtime/data-tag state");
    auto saved=build_scene_asset(world);check(saved.ok(),saved.diagnostics.empty()?"save failed":saved.diagnostics.front());
    check(saved.scene->entities.empty()&&saved.scene->prefabs.size()==2,"structural save unpacked instances");
    std::ostringstream json;write_scene_asset(json,*saved.scene);auto doc=parse_scene_asset(json.str(),world.components());check(doc.ok(),"structural JSON roundtrip failed");
    kin::EcsWorld copy;configure(copy);auto restored=instantiate_scene_asset(copy,*doc.scene,scratch.path,copy.components());
    check(restored.ok(),restored.diagnostics.empty()?"restore failed":restored.diagnostics.front());
    check(restored.by_authored_id.contains("one.extra")&&!copy.components().has(restored.by_authored_id.at("two.gadget"),"Timer"),"saved add/remove lost");
    check(restored.by_authored_id.at("one.gadget").get<RectRenderer>()->visible,"component replacement defaults lost");
    check(!restored.by_authored_id.contains("one.gadget/spark")&&restored.by_authored_id.contains("two.gadget/spark"),"saved removal affected sibling");
    auto second=build_scene_asset(copy);std::ostringstream json2;check(second.ok(),"second structural save failed");write_scene_asset(json2,*second.scene);
    check(json.str()==json2.str(),"structural save is not stable");
    PrefabAssetLoadOptions limits;limits.max_entities=4;
    check(!load_prefab_asset(scratch.path/"outer.kinprefab",world.components(),limits).ok(),"expansion budget ignored");limits={};limits.max_depth=1;
    check(!load_prefab_asset(scratch.path/"outer.kinprefab",world.components(),limits).ok(),"depth budget ignored");limits={};limits.max_sources=1;
    check(!load_prefab_asset(scratch.path/"outer.kinprefab",world.components(),limits).ok(),"source count budget ignored");limits={};limits.max_source_bytes=8;
    check(!load_prefab_asset(scratch.path/"outer.kinprefab",world.components(),limits).ok(),"source byte budget ignored");
    check(!runtime.load(scratch.path/"scene.kinscene",scratch.path,world.components(),{4096,8})&&runtime.entity_count()==9,"failed bounded load replaced source metadata");
    write(scratch.path/"inner.kinprefab",std::string(inner)+"\n");check(runtime.poll_changes()==SceneAssetRuntimeChange::PrefabAsset,"nested source edit not watched");
    Scratch archive;check(write_content_pack(scratch.path,archive.path/"nested.kinpak").ok,"pack write failed");const auto mount=archive.path/"virtual";mount_content_pack(mount,ContentPack::open(archive.path/"nested.kinpak"));
    check(load_prefab_asset(mount/"outer.kinprefab",world.components()).ok(),"packed composition failed");unmount_content_pack(mount);
    write(scratch.path/"inner.kinprefab",R"({"id":"cycle","root":"body","entities":[{"id":"body"}],"prefabs":[{"id":"loop","asset":"outer.kinprefab"}]})");
    const auto cycle=load_prefab_asset(scratch.path/"outer.kinprefab",world.components());check(!cycle.ok()&&cycle.diagnostics.front().find("cycle")!=std::string::npos,"composition cycle not diagnosed");
    auto outside=world.entities().create({.authored_id="outside"});world.entities().reparent(extra,outside);root.destroy();
    check(!build_scene_asset(world).ok()&&build_scene_asset(world,{.preserve_prefabs=false}).ok(),"orphaned members silently dropped or explicit unpack failed");
}
void invalid_edits_and_cache(){
    kin::EcsWorld world;configure(world);auto source=parse_prefab_asset(inner,world.components());check(source.ok(),"fixture invalid");
    PrefabOverrideSet bad;bad.remove_entities={"body"};check(!apply_prefab_overrides(*source.prefab,bad,world.components()).ok(),"root removal accepted");
    bad={};bad.parents["spark"]="ghost";check(!instantiate_prefab(world,*source.prefab,{},world.components(),bad).ok(),"cycle instantiated");
    bad={};bad.add_entities.push_back({.id="body"});check(!apply_prefab_overrides(*source.prefab,bad,world.components()).ok(),"duplicate added ID accepted");
    bad={};bad.remove_components["body"]={"Unknown"};check(!apply_prefab_overrides(*source.prefab,bad,world.components()).ok(),"unknown component removal accepted");
    bad={};bad.add_entities.push_back({.id="detached"});check(!apply_prefab_overrides(*source.prefab,bad,world.components()).ok(),"detached addition accepted but cannot roundtrip");
    bad={};bad.remove_entities={"body-missing"};check(!apply_prefab_overrides(*source.prefab,bad,world.components()).ok(),"unknown entity removal accepted");
    bad={};bad.remove_entities={"ghost"};bad.entities["ghost"]={{"Timer",{}}};check(!apply_prefab_overrides(*source.prefab,bad,world.components()).ok(),"patch to removed entity accepted");
    auto single=parse_prefab_asset(R"({"id":"versions","root":"root","entities":[{"id":"root","components":{"RectRenderer":{"size":[1,1]}}}]})",world.components());
    std::vector<kin::EcsEntity> live;
    for(int i=0;i<128;++i){single.prefab->entities[0].components[0].fields[0].value=Vec2f{f32(i+1),1};auto version=instantiate_prefab(world,*single.prefab,{},world.components());check(version.ok(),"live compiler generations rejected too early");live.push_back(version.root);}
    single.prefab->entities[0].components[0].fields[0].value=Vec2f{999,1};
    check(!instantiate_prefab(world,*single.prefab,{},world.components()).ok()&&live.front().get<RectRenderer>()->size.x==1,"compiler evicted active generation or ignored limit");
    live.front().destroy();check(instantiate_prefab(world,*single.prefab,{},world.components()).ok(),"unused compiler generation was not reclaimed");
}
void asset_manager_root(){
    Scratch scratch;std::filesystem::create_directories(scratch.path/"prefabs");
    write(scratch.path/"prefabs/leaf.kinprefab",R"({"id":"leaf","root":"root","entities":[{"id":"root","components":{"Transform2D":{}}}]})");
    write(scratch.path/"prefabs/outer.kinprefab",R"({"id":"outer","root":"root","entities":[{"id":"root","components":{"Transform2D":{}}}],"prefabs":[{"id":"leaf","asset":"prefabs/leaf.kinprefab"}]})");
    AssetManager assets(scratch.path);register_default_asset_loaders(assets);auto loaded=assets.load<PrefabAsset>("prefabs/outer.kinprefab");
    check(bool(loaded)&&scene_document_from_prefab_asset(*loaded).entities.size()==2,"default asset loader ignored common content root or document conversion omitted composition");
}
}
int main(){try{composition_and_roundtrip();invalid_edits_and_cache();asset_manager_root();std::cout<<"Prefab composition, structural saves, budgets and lifetime passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
