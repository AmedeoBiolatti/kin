#include <kin/scene/scene_session3d.hpp>
#include <kin/assets/content.hpp>
#include <kin/ecs/system.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace kin {
namespace {
struct SceneIdentity3D {u64 session=0,generation=0;};
struct SceneLease3D {u64 instance=0;bool active=false;};
}
EcsEntity SceneEntity3D::resolve(EcsWorld& world)const {
    const auto* identity=world.raw().get<SceneIdentity3D>();
    if(!identity||identity->session!=session||identity->generation!=generation)return {};
    auto result=world.entities().resolve(entity);if(!result)return {};
    if(const auto* lease=result.get<SceneLease3D>()) {
        if(lease->instance!=instance||!lease->active)return {};
    } else if(instance)return {};
    return result;
}
namespace detail {
struct SceneRequestState3D {
    mutable std::mutex mutex;
    SceneRequestStatus3D status=SceneRequestStatus3D::Pending;
    ScenePrefab3D prefab;
    std::string error;
    void finish(SceneRequestStatus3D next,ScenePrefab3D value={},std::string_view message={}) {
        std::lock_guard lock(mutex);
        if(status!=SceneRequestStatus3D::Pending)return;
        prefab=value;error=message;status=next;
    }
};
struct SceneOperation3D {
    std::shared_ptr<SceneRequestState3D> result=std::make_shared<SceneRequestState3D>();
    std::filesystem::path path;
    SceneSpawn3D options;
    ScenePreparedPrefab3D prepared;
    ScenePrefabPool3D pool;
    bool destroy=false,enqueued=false;
    ScenePrefab3D prefab;
    SceneRequest3D source;
    void cancel(const char* reason) {
        result->finish(SceneRequestStatus3D::Cancelled,{},reason);
        options={};path.clear();source={}; // release constructor captures even in an unflushed buffer
    }
    ~SceneOperation3D(){result->finish(SceneRequestStatus3D::Cancelled,{},"scene command discarded before execution");}
};
struct SceneCommandQueue3D {
    std::mutex mutex;
    EcsWorld* world=nullptr; // commands compare at flush; queries read on owner thread
    World3D* physics=nullptr;
    WorldPhysicsState3D* bodies=nullptr;
    const std::thread::id owner=std::this_thread::get_id();
    bool active=false;
    size_t limit=0;
    std::vector<std::weak_ptr<SceneOperation3D>> reserved;
    // Reuse both bounded vectors across steps; an idle drain allocates nothing.
    std::vector<std::shared_ptr<SceneOperation3D>> pending,ready;
    ~SceneCommandQueue3D() {
        for(auto& weak:reserved)if(auto op=weak.lock())
            op->cancel("owning scene was replaced or unloaded");
    }
};
}

namespace {
bool queries_available(const std::shared_ptr<detail::SceneCommandQueue3D>& queue) {
    return queue&&queue->owner==std::this_thread::get_id()&&queue->active&&queue->physics&&queue->bodies;
}
SceneEntity3D checked_scene_entity(EcsWorld& world,EcsEntity entity) {
    if(!entity||entity.raw().world().c_ptr()!=world.raw().c_ptr())return {};
    const auto* identity=world.raw().get<SceneIdentity3D>();if(!identity)return {};
    const auto handle=world.entities().handle(entity);if(!handle.id)return {};
    const auto* lease=entity.get<SceneLease3D>();if(lease&&!lease->active)return {};
    return {identity->session,identity->generation,handle,lease?lease->instance:0};
}
std::optional<QueryFilter3D> query_filter(const detail::SceneCommandQueue3D& queue,const SceneQueryFilter3D& filter) {
    QueryFilter3D raw{.mask=filter.mask,.sensors=filter.sensors,.initial_overlap=filter.initial_overlap};
    // Even a malformed partially filled handle must fail closed.
    if(filter.ignore.session||filter.ignore.generation||filter.ignore.entity.id||filter.ignore.entity.generation||filter.ignore.instance) {
        auto entity=filter.ignore.resolve(*queue.world);if(!entity)return std::nullopt;
        raw.ignore_user_id=entity.id();raw.ignore=queue.bodies->body_of(entity.raw());
        if(!raw.ignore)if(const auto* character=entity.get<PhysicsCharacter3D>())raw.ignore=queue.physics->character(character->character).body;
    }
    return raw;
}
SceneEntity3D query_entity(const detail::SceneCommandQueue3D& queue,Body3D body) {
    const auto entity=queue.bodies->entity_of_body(body);
    return entity?checked_scene_entity(*queue.world,EcsEntity{entity}):SceneEntity3D{};
}
}
SceneQueries3D::operator bool()const {return queries_available(_queue.lock());}
SceneEntity3D SceneQueries3D::handle(EcsEntity entity)const {
    const auto queue=_queue.lock();return queries_available(queue)?checked_scene_entity(*queue->world,entity):SceneEntity3D{};
}
EcsEntity SceneQueries3D::resolve(SceneEntity3D handle)const {
    const auto queue=_queue.lock();return queries_available(queue)?handle.resolve(*queue->world):EcsEntity{};
}
EcsEntity SceneQueries3D::resolve_hit(SceneEntity3D handle,Body3D body)const {
    const auto queue=_queue.lock();if(!queries_available(queue))return {};
    auto entity=handle.resolve(*queue->world);
    return entity&&queue->bodies->entity_of_body(body)==entity.raw()?entity:EcsEntity{};
}
std::optional<SceneRayHit3D> SceneQueries3D::raycast(Vec3f from,Vec3f to,const SceneQueryFilter3D& filter)const {
    const auto queue=_queue.lock();if(!queries_available(queue))return {};
    const auto raw=query_filter(*queue,filter);if(!raw)return {};
    const auto hit=queue->physics->raycast(from,to,*raw);if(!hit)return {};
    return SceneRayHit3D{*hit,query_entity(*queue,hit->body)};
}
BoundedQueryResult3D SceneQueries3D::raycast_all(Vec3f from,Vec3f to,std::span<SceneRayHit3D> output,const SceneQueryFilter3D& filter)const {
    const auto queue=_queue.lock();if(!queries_available(queue))return {.valid=false};
    const auto raw=query_filter(*queue,filter);if(!raw)return {.valid=false};
    const auto result=queue->physics->raycast_into(from,to,{output.data(),output.size(),[](void* data,std::size_t i)->RayHit3D&{return static_cast<SceneRayHit3D*>(data)[i].physics;}},*raw);
    for(std::size_t i=0;i<result.count;++i)output[i].entity=query_entity(*queue,output[i].physics.body);
    return result;
}
std::optional<SceneShapeHit3D> SceneQueries3D::shape_cast(const Shape3D& shape,Vec3f position,Quatf rotation,Vec3f displacement,const SceneQueryFilter3D& filter)const {
    const auto queue=_queue.lock();if(!queries_available(queue))return {};
    const auto raw=query_filter(*queue,filter);if(!raw)return {};
    const auto hit=queue->physics->shape_cast(shape,position,rotation,displacement,*raw);if(!hit)return {};
    return SceneShapeHit3D{*hit,query_entity(*queue,hit->body)};
}
BoundedQueryResult3D SceneQueries3D::overlap(const Shape3D& shape,Vec3f position,Quatf rotation,std::span<SceneOverlapHit3D> output,const SceneQueryFilter3D& filter)const {
    const auto queue=_queue.lock();if(!queries_available(queue))return {.valid=false};
    const auto raw=query_filter(*queue,filter);if(!raw)return {.valid=false};
    const auto result=queue->physics->overlap_into(shape,position,rotation,{output.data(),output.size(),[](void* data,std::size_t i)->OverlapHit3D&{return static_cast<SceneOverlapHit3D*>(data)[i].physics;}},*raw);
    for(std::size_t i=0;i<result.count;++i)output[i].entity=query_entity(*queue,output[i].physics.body);
    return result;
}

SceneRequestStatus3D SceneRequest3D::status() const {
    if(!_state)return SceneRequestStatus3D::Invalid;
    std::lock_guard lock(_state->mutex);return _state->status;
}
ScenePrefab3D SceneRequest3D::prefab() const {
    if(!_state)return {};
    std::lock_guard lock(_state->mutex);return _state->prefab;
}
std::string SceneRequest3D::error() const {
    if(!_state)return "invalid scene request";
    std::lock_guard lock(_state->mutex);return _state->error;
}
SceneRequest3D SceneCommands3D::submit(EcsCommandBuffer& buffer,std::shared_ptr<detail::SceneOperation3D> op) const {
    SceneRequest3D ticket;ticket._state=op->result;
    const auto queue=_queue.lock();
    if(!queue){op->result->finish(SceneRequestStatus3D::Cancelled,{},"owning scene is no longer available");return ticket;}
    {
        std::lock_guard lock(queue->mutex);
        std::erase_if(queue->reserved,[](const auto& weak){
            const auto op=weak.lock();if(!op)return true;
            std::lock_guard lock(op->result->mutex);return op->result->status!=SceneRequestStatus3D::Pending;
        });
        if(queue->reserved.size()>=queue->limit) {
            op->result->finish(SceneRequestStatus3D::Failed,{},"pending scene command budget exceeded");return ticket;
        }
        queue->reserved.push_back(op);
    }
    buffer.defer([weak=_queue,op=std::move(op)](EcsWorld& world,std::string&) mutable {
        if(!op)return true; // a manually retried partial flush must not enqueue twice
        const auto queue=weak.lock();
        if(!queue)op->result->finish(SceneRequestStatus3D::Cancelled,{},"owning scene is no longer available");
        else {
            std::lock_guard lock(queue->mutex);
            bool pending;
            {std::lock_guard result_lock(op->result->mutex);pending=op->result->status==SceneRequestStatus3D::Pending;}
            // A copied/retried buffer cannot execute a request twice.
            if(pending&&!op->enqueued) {
                if(!queue->active||queue->world!=&world)
                    op->result->finish(SceneRequestStatus3D::Failed,{},"scene command flushed against an inactive or foreign world");
                else {queue->pending.push_back(op);op->enqueued=true;}
            }
        }
        op.reset();return true; // asynchronous failures belong to the ticket
    });
    return ticket;
}
SceneRequest3D SceneCommands3D::spawn(EcsCommandBuffer& buffer,const std::filesystem::path& path,SceneSpawn3D options) const {
    auto op=std::make_shared<detail::SceneOperation3D>();
    if(path.empty()||path.native().size()>4096) {
        SceneRequest3D ticket;ticket._state=op->result;
        op->result->finish(SceneRequestStatus3D::Failed,{},"prefab path is empty or exceeds 4096 bytes");return ticket;
    }
    op->path=path;op->options=std::move(options);return submit(buffer,std::move(op));
}
SceneRequest3D SceneCommands3D::despawn(EcsCommandBuffer& buffer,ScenePrefab3D prefab) const {
    auto op=std::make_shared<detail::SceneOperation3D>();op->destroy=true;op->prefab=prefab;return submit(buffer,std::move(op));
}
SceneRequest3D SceneCommands3D::spawn(EcsCommandBuffer& buffer,ScenePreparedPrefab3D prefab,SceneSpawn3D options) const {
    auto op=std::make_shared<detail::SceneOperation3D>();op->prepared=prefab;op->options=std::move(options);return submit(buffer,std::move(op));
}
SceneRequest3D SceneCommands3D::spawn(EcsCommandBuffer& buffer,ScenePrefabPool3D pool,SceneSpawn3D options) const {
    auto op=std::make_shared<detail::SceneOperation3D>();op->pool=pool;op->options=std::move(options);return submit(buffer,std::move(op));
}
SceneRequest3D SceneCommands3D::despawn(EcsCommandBuffer& buffer,SceneRequest3D spawn) const {
    auto op=std::make_shared<detail::SceneOperation3D>();op->destroy=true;op->source=std::move(spawn);return submit(buffer,std::move(op));
}

namespace {
std::atomic<u64> next_session_id{1};
struct BoolScope {
    bool& value;
    bool previous;
    explicit BoolScope(bool& value):value(value),previous(value){value=true;}
    ~BoolScope(){value=previous;}
};
bool same_options(const View3DOptions& a, const View3DOptions& b) {
    return a.samples == b.samples && a.background == b.background && a.hdr == b.hdr && a.exposure == b.exposure &&
        a.bloom == b.bloom && a.bloom_threshold == b.bloom_threshold && a.pick == b.pick;
}
struct SourceStamp {
    std::optional<std::filesystem::file_time_type> time;
    std::optional<u64> size;
    std::weak_ptr<const ContentPack> pack; // identity only; does not pin archive mappings
    bool operator==(const SourceStamp& other) const {
        return time == other.time && size == other.size &&
            !pack.owner_before(other.pack) && !other.pack.owner_before(pack);
    }
};
SourceStamp stamp(const std::filesystem::path& source) {
    const auto pack = content_pack_for(source);
    const auto disk = pack ? pack->file() : source;
    std::error_code error;
    const auto time = std::filesystem::last_write_time(disk, error);
    SourceStamp result;
    if (!error) result.time = time;
    result.size = content_file_size(source);
    result.pack = pack;
    return result;
}
void append(std::vector<std::string>& to, const std::vector<std::string>& from) {
    to.insert(to.end(), from.begin(), from.end());
}
constexpr size_t max_edit_text = 64u * 1024u;
size_t text_bytes(const ComponentFieldValue& value) {
    if(const auto* text=std::get_if<std::string>(&value))return text->size();
    if(const auto* asset=std::get_if<ComponentAssetRef>(&value))return asset->id.size();
    if(const auto* entity=std::get_if<ComponentEntityRef>(&value))return entity->authored_id.size();
    return 0;
}
}

struct SceneSession3D::Data {
    struct State {
        struct Spawned {
            ScenePrefab3D handle;
            std::vector<EcsEntityHandle> members;
            std::unordered_map<std::string,EcsEntityHandle> local;
            bool inactive=false,reference=false;
            u64 pool=0;
        };
        struct Prepared { PrefabAsset recipe; u64 reference=0; };
        struct Pool {
            u64 prepared=0;
            ScenePrefabPoolOptions3D options;
            std::vector<u64> slots;
            u64 acquired=0,reused=0,exhausted=0;
            std::set<EcsId> used;
        };
        // Keep physics alive while game components in world are destroyed.
        std::unique_ptr<World3D> physics;
        EcsWorld world;
        SceneAssetRuntime asset;
        std::unique_ptr<WorldRenderState3D> rendering;
        std::unique_ptr<WorldPhysicsState3D> bodies;
        Scene3D scene;
        View3D view;
        View3DOptions options;
        std::vector<std::filesystem::path> dependencies;
        std::map<std::filesystem::path, SourceStamp> watches;
        std::map<u64,Spawned> spawned;
        std::unordered_map<EcsId,u64> owners;
        std::map<u64,Prepared> prepared;
        std::map<u64,Pool> pools;
        std::shared_ptr<detail::SceneCommandQueue3D> commands=std::make_shared<detail::SceneCommandQueue3D>();

        SceneSession3DContext context() {
            return {world, asset.instance(), *rendering, physics.get(), bodies.get(), dependencies,SceneCommands3D{commands},SceneQueries3D{commands}};
        }
    };
    Renderer3D& renderer;
    Assets3D& assets;
    SceneSession3DOptions options;
    PhysicsAssets3D collision_assets;
    std::unique_ptr<State> current, pending;
    std::vector<std::string> errors;
    u64 generation = 0;
    const u64 identity = next_session_id.fetch_add(1);
    u64 next_spawn = 1;
    u64 next_resource = 1;
    bool editing = false;
    bool in_frame = false;

    bool gameplay_boundary() {
        if(!current || pending || editing || in_frame || ecs_is_deferred(current->world.raw().c_ptr()) ||
            ecs_stage_is_readonly(current->world.raw().c_ptr())) {
            errors={"runtime lifecycle/configuration requires an active scene between frames, outside deferred iteration and with no prepared replacement"};
            return false;
        }
        return true;
    }

    Data(Renderer3D& renderer, Assets3D& assets, SceneSession3DOptions options)
        : renderer(renderer), assets(assets), options(std::move(options)), collision_assets(assets.root(),this->options.collision_limits) {}

    // Inactive slots keep ECS storage and bound resources, but never participate
    // in queries, rendering or contacts. The bridge removes ordinary native
    // bodies while idle and recreates them from reset descriptors on activation.
    void idle(State& state,State::Spawned& record) {
        BoolScope busy(editing);
        std::vector<EcsId> ids;
        for(auto h:record.members)if(auto e=state.world.entities().resolve(h)){e.disable();e.set(SceneLease3D{record.handle.id,false});ids.push_back(e.id());}
        record.inactive=true;
        if(state.bodies)state.bodies->sync_entities(ids);
        state.scene.clear();state.rendering->clear_collection_cache();
    }
    SceneSpawnContext3D context(State& state,State::Spawned& record,std::unordered_map<std::string,EcsEntity>& local) {
        for(const auto& [name,h]:record.local) {
            auto e=state.world.entities().resolve(h);if(!e)throw std::runtime_error("pool member was destroyed directly");
            local.emplace(name,e);
        }
        return {state.world,state.world.entities().resolve(record.handle.root.entity),local};
    }
    template<class Resolve>
    void copy_bound(State& state,EcsEntity src,EcsEntity dst,const std::string& name,const State::Spawned& reference,Resolve target) {
        auto& registry=state.world.components();
        if(!registry.copy_component(src,name,registry,dst))throw std::runtime_error(registry.last_error());
        const auto* descriptor=registry.find(name);
        if(std::ranges::none_of(descriptor->fields,[](const auto& field){return field.kind==ComponentFieldKind::EntityRef;}))return;
        const auto snapshot=registry.snapshot(dst,name);
        for(const auto& field:snapshot->fields)if(const auto* ref=std::get_if<ComponentEntityRef>(&field.value))
            for(const auto& [local,handle]:reference.local)if(ref->id==handle.id) {
                auto value=*ref;value.id=target(local).id();
                if(!registry.patch_field(dst,name,field.name,value))throw std::runtime_error(registry.last_error());
                break;
            }
    }
    void fixed_assets(EcsEntity src,EcsEntity dst) {
        const auto same=[&]<class T>(auto equal) {
            const auto* a=src.get<T>();const auto* b=dst.get<T>();
            if(bool(a)!=bool(b)||(a&&!equal(*a,*b)))throw std::runtime_error("prepared/pool asset configuration changed; prepare another prefab instead");
        };
        same.template operator()<Transform3D>([](const auto& a,const auto& b){return a.scale==b.scale;});
        same.template operator()<ModelRenderer3D>([](const auto& a,const auto& b){return a.asset==b.asset&&a.model==b.model;});
        same.template operator()<SkinnedMeshRenderer>([](const auto& a,const auto& b){return a.asset==b.asset&&a.model==b.model;});
        same.template operator()<ModelMaterials3D>([](const auto& a,const auto& b){return a.asset==b.asset&&a.variant==b.variant&&a.binding==b.binding;});
        same.template operator()<AnimatorAsset3D>([](const auto& a,const auto& b){return a.asset==b.asset&&a.configuration==b.configuration;});
        same.template operator()<ParticleEmitter3D>([](const auto& a,const auto& b){return a.texture_asset==b.texture_asset&&a.texture_nearest==b.texture_nearest&&a.system.desc.look.texture==b.system.desc.look.texture;});
        same.template operator()<Collider3D>([&](const auto& a,const auto& b){return same_collider(a,b);});
        if(dst.has<SceneCamera3D>()||dst.has<SceneEnvironment3D>())throw std::runtime_error("pool objects cannot replace scene settings");
    }
    bool same_collider(const Collider3D& a,const Collider3D& b) {
        return a.asset==b.asset&&a.asset_binding==b.asset_binding&&a.kind==b.kind&&a.half_extents==b.half_extents&&
            a.radius==b.radius&&a.half_height==b.half_height&&a.mesh==b.mesh&&a.offset==b.offset&&a.rotation==b.rotation&&
            a.scale==b.scale&&a.shape==b.shape&&std::ranges::equal(a.children,b.children,[&](const auto& x,const auto& y){return same_collider(x,y);});
    }
    void restore(State& state,State::Spawned& record,const State::Prepared& prepared) {
        const auto& reference=state.spawned.at(prepared.reference);
        for(const auto& entity:resolved_prefab_asset(prepared.recipe).entities) {
            auto src=state.world.entities().resolve(reference.local.at(entity.id));
            auto dst=state.world.entities().resolve(record.local.at(entity.id));
            if(!src||!dst)throw std::runtime_error("preloaded/pool reference member was destroyed directly");
            const auto remove_extra=[&]<class T>(){if(!src.has<T>()&&dst.has<T>())dst.remove<T>();};
            remove_extra.template operator()<ModelRenderer3D>();remove_extra.template operator()<SkinnedMeshRenderer>();
            remove_extra.template operator()<ModelMaterials3D>();remove_extra.template operator()<AnimatorAsset3D>();
            remove_extra.template operator()<Animator3D>();remove_extra.template operator()<ParticleEmitter3D>();
            remove_extra.template operator()<Collider3D>();remove_extra.template operator()<RigidBody3D>();
            remove_extra.template operator()<CharacterController3D>();remove_extra.template operator()<Ragdoll3D>();remove_extra.template operator()<Rope3D>();
            remove_extra.template operator()<SceneCamera3D>();remove_extra.template operator()<SceneEnvironment3D>();
            for(const auto& component:entity.components)copy_bound(state,src,dst,component.name,reference,
                [&](const auto& local){return state.world.entities().resolve(record.local.at(local));});
            // PhysicsBody3D is derived, not part of the source configuration.
            dst.remove<PhysicsBody3D>();
            dst.remove<RenderTransform3D>();
            dst.remove<JointAttachment3D>();
            // Transform's OnAdd observer runs only on construction. Keep this
            // derived component present so reused slots still enter propagation,
            // model/light collection and particle simulation queries.
            if(dst.has<Transform3D>())dst.set(WorldTransform3D{});
        }
    }
    void fixed_members(State& state,const State::Spawned& record,const State::Prepared& prepared) {
        for(const auto& entity:resolved_prefab_asset(prepared.recipe).entities) {
            auto e=state.world.entities().resolve(record.local.at(entity.id));
            if(!e)throw std::runtime_error("pool member was destroyed directly");
            const auto parent=entity.parent.empty()?0:record.local.at(entity.parent).id;
            if(e.parent().id()!=parent)throw std::runtime_error("pool hierarchy was reparented");
            e.raw().children([&](flecs::entity child){if(std::ranges::none_of(record.members,[&](auto h){return h.id==child.id();}))
                throw std::runtime_error("pool slot has an unmanaged child");});
        }
    }
    void renew(State& state,State::Spawned& record) {
        const auto old=record.handle.id,id=next_spawn++;
        record.handle.id=id;record.handle.root.instance=id;
        for(auto h:record.members){state.owners[h.id]=id;if(auto e=state.world.entities().resolve(h))e.set(SceneLease3D{id,false});}
        if(record.pool)for(auto& slot:state.pools.at(record.pool).slots)if(slot==old){slot=id;break;}
        auto node=state.spawned.extract(old);node.key()=id;state.spawned.insert(std::move(node));
    }

    void watch(std::map<std::filesystem::path,SourceStamp>& watches, const std::filesystem::path& source) {
        if (source.empty()) return;
        const auto path = (assets.root() / source).lexically_normal();
        if (!watches.contains(path) && watches.size() >= options.max_dependencies)
            throw std::runtime_error("scene exceeds dependency limit");
        watches[path] = stamp(path);
    }
    void watch(State& state,const std::filesystem::path& source){watch(state.watches,source);}

    void gather(State& state) {
        watch(state, state.asset.scene_path());
        for (const auto& prefab : state.asset.scene()->prefabs) watch(state, prefab.asset);
        for(const auto& source:state.asset.prefab_sources())watch(state,source);
        const auto models = [&]<class T>() {
            state.world.raw().each([&](flecs::entity, const T& renderer) {
                if (!renderer.asset.id.empty()) watch(state, renderer.asset.id);
                if (renderer.model) {
                    append(errors, renderer.model->asset_diagnostics);
                    for (const auto& source : renderer.model->source_files) {
                        watch(state, source);
                        if (!content_file_exists(source)) errors.push_back("missing model dependency: " + source.string());
                    }
                }
            });
        };
        models.operator()<ModelRenderer3D>(); models.operator()<SkinnedMeshRenderer>();
        state.world.raw().each([&](flecs::entity, const Collider3D& collider) {
            if(!collider.asset.id.empty()) watch(state,collider.asset.id);
            if(collider.asset_binding) for(const auto& source:collider.asset_binding->source_files) watch(state,source);
        });
        state.world.raw().each([&](flecs::entity,const ParticleEmitter3D& emitter) {
            if(!emitter.texture_asset.id.empty())watch(state,emitter.texture_asset.id);
            if(!emitter.bound_texture.empty())watch(state,emitter.bound_texture);
        });
        state.world.raw().each([&](flecs::entity, const ModelMaterials3D& material) {
            if (!material.asset.id.empty()) watch(state, material.asset.id);
            if (material.binding) for (const auto& source : material.binding->source_files) watch(state, source);
        });
        state.world.raw().each([&](flecs::entity, const AnimatorAsset3D& animator) {
            if (!animator.asset.id.empty()) watch(state, animator.asset.id);
            if (animator.configuration) watch(state, animator.configuration->machine_asset);
        });
        for (const auto& source : state.dependencies) watch(state, source);
    }

    // Live batches contribute only their own bindings. Existing stamps stay
    // unchanged so an edit cannot accidentally acknowledge an unrelated reload.
    void gather_entity(std::map<std::filesystem::path,SourceStamp>& state, EcsEntity entity) {
        const auto model = [&]<class T>() {
            if (const auto* c = entity.get<T>()) {
                watch(state,c->asset.id);
                if (c->model) {
                    append(errors,c->model->asset_diagnostics);
                    for (const auto& file:c->model->source_files) {
                        watch(state,file);
                        if (!content_file_exists(file)) errors.push_back("missing model dependency: "+file.string());
                    }
                }
            }
        };
        model.operator()<ModelRenderer3D>(); model.operator()<SkinnedMeshRenderer>();
        if (const auto* c=entity.get<ModelMaterials3D>()) {
            watch(state,c->asset.id);
            if(c->binding)for(const auto& file:c->binding->source_files)watch(state,file);
        }
        if (const auto* c=entity.get<AnimatorAsset3D>()) {
            watch(state,c->asset.id);
            if(c->configuration)watch(state,c->configuration->machine_asset);
        }
        if (const auto* c=entity.get<Collider3D>()) {
            watch(state,c->asset.id);
            if(c->asset_binding)for(const auto& file:c->asset_binding->source_files)watch(state,file);
        }
        if (const auto* c=entity.get<ParticleEmitter3D>())watch(state,c->texture_asset.id);
    }

    bool collect(State& state) {
        append(errors,validate_particles3d(state.world,options.particle_limits));
        if(!errors.empty())return false;
        state.scene.clear();
        state.scene.camera = {};
        state.scene.lights = {};
        state.scene.sky = {}; state.scene.fog = {}; state.scene.weather = {};
        state.scene.ambient_occlusion = {}; state.scene.contact_shadows = {}; state.scene.taa = {}; state.scene.lod = {};
        state.scene.cull = true;
        state.rendering->propagate_transforms();
        auto view_options = options.view;
        append(errors, apply_scene3d_settings(state.world, state.scene, &view_options));
        if (!errors.empty()) return false;
        state.rendering->apply_time_of_day(state.scene);
        const auto base_fog = state.scene.fog;
        state.rendering->apply_weather(state.scene, base_fog);
        state.rendering->collect(state.scene);
        if (state.bodies) state.bodies->collect_ropes(renderer, state.scene);
        if (options.compose) { auto context = state.context(); options.compose(context, state.scene); }
        if (!state.view || state.view.size() != options.viewport || !same_options(state.options, view_options)) {
            View3D replacement;
            // Sharing an unchanged target avoids double frame buffers during reload.
            if (current && current.get() != &state && current->view.size() == options.viewport && same_options(current->options, view_options))
                replacement = current->view;
            else {
                replacement = renderer.create_view(options.viewport, view_options);
                if (!replacement) { errors.push_back("scene view creation failed"); return false; }
                if (options.warm_up && !renderer.warm_up(replacement, state.scene)) {
                    errors.push_back("scene GPU warm-up failed"); return false;
                }
            }
            state.view = std::move(replacement); state.options = view_options;
        }
        return true;
    }

    void verify_physics(State& state) {
        if (!state.bodies) return;
        state.bodies->sync_bodies();
        state.world.raw().each([&](flecs::entity entity, const Transform3D&, const Collider3D&) {
            if (!entity.has<CharacterController3D>() && !state.bodies->body_of(entity))
                errors.push_back("physics body creation failed for entity '" + EcsEntity{entity}.name() + "'");
        });
        state.world.raw().each([&](flecs::entity entity, const CharacterController3D&) {
            if (!state.bodies->character_of(entity)) errors.push_back("character creation failed for entity '" + EcsEntity{entity}.name() + "'");
        });
        state.world.raw().each([&](flecs::entity entity, const Ragdoll3D&) {
            if (!state.bodies->ragdoll_of(entity)) errors.push_back("ragdoll creation failed for entity '" + EcsEntity{entity}.name() + "'");
        });
        state.world.raw().each([&](flecs::entity entity, const Rope3D&) {
            if (!state.bodies->rope_of(entity)) errors.push_back("rope creation failed for entity '" + EcsEntity{entity}.name() + "'");
        });
    }
};

SceneSession3D::SceneSession3D(Renderer3D& renderer, Assets3D& assets, SceneSession3DOptions options)
    : _data(std::make_unique<Data>(renderer, assets, std::move(options))) {
    const auto& config = _data->options;
    if (config.viewport.x <= 0 || config.viewport.y <= 0 || !config.max_dependencies || !config.max_edit_entities || !config.max_edit_fields ||
        !config.max_spawn_entities || !config.max_runtime_entities || !config.max_runtime_prefabs || !config.max_pending_commands ||
        !config.max_preloaded_prefabs || !config.max_prefab_pools)
        throw std::invalid_argument("scene session requires a positive viewport and dependency limit");
    if (config.physics && (!config.physics->max_bodies || !config.physics->max_body_pairs || !config.physics->max_contact_constraints ||
        !config.physics->temp_memory_bytes || config.physics->collision_steps <= 0))
        throw std::invalid_argument("scene session requires positive physics capacities");
    if (config.physics && (!std::isfinite(config.physics->gravity.x) || !std::isfinite(config.physics->gravity.y) ||
        !std::isfinite(config.physics->gravity.z) || !std::isfinite(config.bodies.fixed_dt) || config.bodies.fixed_dt <= 0 || config.bodies.max_steps <= 0))
        throw std::invalid_argument("scene session requires finite gravity and positive physics stepping options");
}
SceneSession3D::~SceneSession3D() { unload(); }

bool SceneSession3D::prepare(const std::filesystem::path& path, bool refresh_all_assets) {
    if(_data->editing||_data->in_frame)return configuration_error("scene preparation cannot run inside a lifecycle/configuration/frame callback");
    BoolScope busy(_data->editing);
    auto& d = *_data; d.errors.clear(); d.pending.reset(); d.assets.trim();
    try {
        if (!d.renderer.available()) throw std::runtime_error("3D renderer unavailable");
        auto candidate = std::make_unique<Data::State>();
        candidate->commands->world=&candidate->world;candidate->commands->limit=d.options.max_pending_commands;
        register_builtin_editor_components(candidate->world);
        if (d.options.register_components) d.options.register_components(candidate->world);
        const auto source = (d.assets.root() / path).lexically_normal();
        if (!candidate->asset.load(source, d.assets.root(), candidate->world.components(), d.options.limits)) {
            append(d.errors, candidate->asset.diagnostics()); return false;
        }
        if (!candidate->asset.scene()->script.empty()) throw std::runtime_error("SceneSession3D requires a C++ scene; use ScriptScene for Lua scene lifecycle");
        if (refresh_all_assets) { d.assets.clear();d.collision_assets.clear(); }
        else if (d.current) {
            const auto changed = changed_sources();
            for (const auto& file : changed) { d.assets.invalidate(file);d.collision_assets.invalidate(file); }
            // Also invalidate owning models if bounded cache metadata was evicted.
            const auto invalidate = [&]<class T>() {
                d.current->world.raw().each([&](flecs::entity, const T& component) {
                    if (!component.model || component.asset.id.empty()) return;
                    for (const auto& file : changed) if (std::ranges::find(component.model->source_files, file) != component.model->source_files.end()) {
                        d.assets.invalidate(component.asset.id); break;
                    }
                });
            };
            invalidate.operator()<ModelRenderer3D>(); invalidate.operator()<SkinnedMeshRenderer>();
        }
        candidate->rendering = std::make_unique<WorldRenderState3D>(candidate->world);
        if (!candidate->asset.instantiate(candidate->world)) { append(d.errors, candidate->asset.diagnostics()); return false; }
        append(d.errors, resolve_render3d_assets(candidate->world, d.assets,d.options.particle_limits));
        if (!d.errors.empty()) return false;
        if (d.options.physics) {
            candidate->physics = std::make_unique<World3D>(*d.options.physics);
            candidate->bodies = std::make_unique<WorldPhysicsState3D>(candidate->world, *candidate->physics, d.options.bodies);
        }
        candidate->commands->physics=candidate->physics.get();candidate->commands->bodies=candidate->bodies.get();
        if (d.options.initialize) { auto context = candidate->context(); d.options.initialize(context); }
        if (d.options.limits.max_entities && candidate->world.entities().all().size() > d.options.limits.max_entities)
            throw std::runtime_error("initialization exceeds scene entity limit");
        // Bind any authored references added by initialization, before physics.
        append(d.errors, resolve_render3d_assets(candidate->world, d.assets,d.options.particle_limits));
        if (!d.errors.empty()) return false;
        if(candidate->bodies) append(d.errors,resolve_physics3d_assets(candidate->world,d.collision_assets));
        if (!d.errors.empty()) return false;
        d.gather(*candidate);
        if (!d.errors.empty()) return false;
        d.verify_physics(*candidate);
        if (!d.errors.empty()) return false;
        if (!d.collect(*candidate)) return false;
        d.pending = std::move(candidate);
        return true;
    } catch (const std::exception& error) { d.errors.push_back("scene preparation: " + std::string(error.what())); }
    return false;
}

bool SceneSession3D::activate() {
    if(_data->editing||_data->in_frame)return configuration_error("scene activation cannot run inside a callback");
    auto& d = *_data; d.errors.clear();
    if (!d.pending) { d.errors.push_back("no prepared scene to activate"); return false; }
    d.current = std::move(d.pending); ++d.generation;
    d.current->world.raw().set(SceneIdentity3D{d.identity,d.generation});
    {std::lock_guard lock(d.current->commands->mutex);d.current->commands->active=true;}
    d.current->world.components().set_field_patch_handler([this](EcsEntity entity, std::string_view component,
        std::string_view field, const ComponentFieldValue& value, std::string& error) -> std::optional<bool> {
        // Prefab compilation and spawning build incomplete entities through the
        // registry too. Intercept only members of the activated authored scene;
        // runtime-created entities can opt in explicitly through edit().
        const auto id=_data->current->world.entities().authored_id(entity);
        if(!id || !_data->current->asset.instance().by_authored_id.contains(*id))return std::nullopt;
        if(component.size()>max_edit_text || field.size()>max_edit_text || text_bytes(value)>max_edit_text) {
            error="live edit exceeds 64 KiB text budget";return false;
        }
        const SceneFieldEdit3D change{entity,std::string(component),std::string(field),value};
        if (edit(std::span(&change,1))) return true;
        for (const auto& message:diagnostics()) { if(!error.empty())error+='\n'; error+=message; }
        return false;
    });
    d.current->view.reset_history(); // new scene, even when reusing its target
    d.assets.trim(); d.collision_assets.trim(); return true;
}
bool SceneSession3D::load(const std::filesystem::path& path, bool refresh) { return prepare(path, refresh) && activate(); }
void SceneSession3D::cancel() {
    if(_data->editing||_data->in_frame){configuration_error("scene cancellation cannot run inside a callback");return;}
    _data->pending.reset(); _data->assets.trim(); _data->collision_assets.trim();
}
void SceneSession3D::unload() {
    if (!_data) return;
    if(_data->editing||_data->in_frame){configuration_error("scene unload cannot run inside a callback");return;}
    auto& d = *_data;
    if (d.current || d.pending) ++d.generation;
    d.pending.reset(); d.current.reset(); d.assets.trim(); d.collision_assets.trim(); d.errors.clear();
}
bool SceneSession3D::reload(bool refresh) {
    if (!_data->current) { _data->errors = {"no active scene to reload"}; return false; }
    const auto path = _data->current->asset.scene_path(); return load(path, refresh);
}
std::vector<std::filesystem::path> SceneSession3D::changed_sources() const {
    std::vector<std::filesystem::path> changed;
    if (_data->current) for (const auto& [file, before] : _data->current->watches)
        if (!(stamp(file) == before)) changed.push_back(file);
    return changed;
}
SceneSession3DReload SceneSession3D::reload_if_changed() {
    if (changed_sources().empty()) return SceneSession3DReload::Unchanged;
    return reload() ? SceneSession3DReload::Activated : SceneSession3DReload::Failed;
}
bool SceneSession3D::edit(std::span<const SceneFieldEdit3D> edits) {
    auto& d=*_data;
    if(d.editing)return false;
    d.errors.clear();
    if(!d.current || d.pending || edits.empty() || edits.size()>d.options.max_edit_fields) {
        d.errors={"live edit requires an active scene, no prepared replacement and a bounded nonempty field batch"};return false;
    }
    auto& active=*d.current;
    if(ecs_is_deferred(active.world.raw().c_ptr()) || ecs_stage_is_readonly(active.world.raw().c_ptr())) {
        d.errors={"live edits must run between ECS steps, outside deferred/readonly iteration"};return false;
    }
    struct Guard { bool& busy; Guard(bool& busy):busy(busy){busy=true;} ~Guard(){busy=false;} } guard(d.editing);
    try {
        size_t text=0;
        for(const auto& edit:edits)for(const auto size:{edit.component.size(),edit.field.size(),text_bytes(edit.value)}) {
            if(size>max_edit_text-text)throw std::runtime_error("live edit exceeds 64 KiB text budget");
            text+=size;
        }
        Data::State staging;
        register_builtin_editor_components(staging.world);
        if(d.options.register_components)d.options.register_components(staging.world);
        struct Entry { EcsEntity source,copy;std::set<std::string> changed;bool full=false,pose=false,move=false,body=false; };
        std::map<EcsId,Entry> entries;
        const auto require=[](bool valid,const std::string& why){if(!valid)throw std::runtime_error(why);};
        // Copy ancestors as transforms only. Descendants are discovered through
        // ChildOf, never by copying/scanning the complete scene.
        std::function<Entry&(EcsEntity,bool)> stage=[&](EcsEntity entity,bool full)->Entry& {
            require(entity.raw().world().c_ptr()==active.world.raw().c_ptr() && entity,"edit entity is stale or belongs to another world");
            require(!entity.raw().has(flecs::Prefab),"live edits cannot target prefab definitions");
            auto found=entries.find(entity.id());
            if(found==entries.end()) {
                require(entries.size()<d.options.max_edit_entities,"live edit exceeds affected entity/ancestor budget");
                Entry entry{entity,staging.world.entity()};
                entry.copy.raw().set_name((entity.name()+"#"+std::to_string(entity.id())).c_str());
                found=entries.emplace(entity.id(),std::move(entry)).first;
                if(auto parent=entity.parent())found->second.copy.child_of(stage(parent,false).copy);
                if(entity.has<Transform3D>())require(active.world.components().copy_component(entity,"Transform3D",staging.world.components(),found->second.copy),staging.world.components().last_error());
            }
            auto& entry=found->second;
            if(full&&!entry.full) {
                for(const auto& descriptor:active.world.components().descriptors())
                    if(active.world.components().has(entity,descriptor.id))
                        require(active.world.components().copy_component(entity,descriptor.name,staging.world.components(),entry.copy),staging.world.components().last_error());
                entry.full=true;
                // These native body annotations are intentionally unreflected.
                if(const auto* value=entity.get<SurfaceMaterial3D>())entry.copy.set(*value);
                if(const auto* value=entity.get<Hold3D>())entry.copy.set(*value);
            }
            return entry;
        };
        for(const auto& edit:edits) {
            auto& entry=stage(edit.entity,true);
            require(entry.source.raw().enabled(),"editing disabled entities requires scene reload");
            require(staging.world.components().patch_field(entry.copy,edit.component,edit.field,edit.value),staging.world.components().last_error());
            entry.changed.insert(edit.component);
        }
        std::function<void(EcsEntity,bool)> descendants=[&](EcsEntity entity,bool move) {
            auto& entry=stage(entity,true);entry.pose=true;entry.move|=move;
            entity.raw().children([&](flecs::entity child){descendants(EcsEntity{child},true);});
        };
        for(const auto& edit:edits)if(edit.component=="Transform3D")descendants(edit.entity,edit.field!="scale");
        bool models=false,materials=false,animation=false,particles=false,physics=false;
        for(auto& [id,entry]:entries) {
            if(!entry.full)continue;
            const auto has=[&](const char* name){return entry.changed.contains(name);};
            for(const auto& name:entry.changed) {
                const auto snapshot=staging.world.components().snapshot(entry.copy,name);
                if(snapshot)for(const auto& field:snapshot->fields) {
                    if(const auto* number=std::get_if<f32>(&field.value))require(std::isfinite(*number),"live scalar fields must be finite");
                    if(const auto* number=std::get_if<f64>(&field.value))require(std::isfinite(*number),"live scalar fields must be finite");
                }
            }
            if(has("RigidBody3D")) {
                const auto& body=*entry.copy.get<RigidBody3D>();
                require(body.mass>=0&&body.density>=0&&(body.mass>0||body.density>0)&&body.friction>=0&&body.restitution>=0&&body.restitution<=1&&
                    body.linear_damping>=0&&body.angular_damping>=0,"invalid live rigid-body mass/material/damping settings");
            }
            models|=has("ModelRenderer3D")||has("SkinnedMeshRenderer");materials|=has("ModelMaterials3D");
            animation|=has("AnimatorAsset3D");particles|=has("ParticleEmitter3D");
            entry.body=d.options.physics && (entry.pose||has("Collider3D")||has("RigidBody3D"));
            if(d.options.physics && (has("CharacterController3D")||has("Ragdoll3D")||has("Rope3D") ||
                (entry.pose && (entry.copy.has<CharacterController3D>()||entry.copy.has<Ragdoll3D>()||entry.copy.has<Rope3D>())) ||
                (has("SkinnedMeshRenderer")&&entry.copy.has<Ragdoll3D>())))
                throw std::runtime_error("controller/ragdoll/rope physics edits require scene reload");
            physics|=entry.body;
            if(entry.pose || has("Collider3D")) {
                const auto positive=[](Vec3f v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&v.x>0&&v.y>0&&v.z>0;};
                if(const auto* t=entry.copy.get<Transform3D>())require(positive(t->scale),"live transforms require positive finite scale");
                if(const auto* c=entry.copy.get<Collider3D>())require(positive(c->scale),"live colliders require positive finite scale");
            }
        }
        if(models)append(d.errors,resolve_render3d_assets(staging.world,d.assets,d.options.particle_limits));
        else {
            if(materials)append(d.errors,resolve_materials3d_assets(staging.world,d.assets));
            if(animation)append(d.errors,resolve_animator3d_assets(staging.world,d.assets.root()));
            if(particles)append(d.errors,resolve_particles3d_assets(staging.world,d.assets,d.options.particle_limits));
        }
        // Combine staged capacities with untouched live emitters without copying
        // their particle arrays or retaining a second world-sized snapshot.
        if(particles||models) {
            append(d.errors,validate_particles3d(staging.world,d.options.particle_limits));
            u64 capacity=0;size_t emitters=0;
            active.world.raw().each([&](flecs::entity e,const ParticleEmitter3D& original){
                const auto it=entries.find(e.id());
                const auto* emitter=it!=entries.end()&&it->second.full?it->second.copy.get<ParticleEmitter3D>():&original;
                ++emitters;capacity+=emitter->system.desc.max_particles;
            });
            require(emitters<=d.options.particle_limits.emitters&&capacity<=d.options.particle_limits.total_particles,"edited scene exceeds total particle/emitter budget");
        }
        if(physics)append(d.errors,resolve_physics3d_assets(staging.world,d.collision_assets));
        WorldRenderState3D validation(staging.world);validation.propagate_transforms();
        for(auto& [id,entry]:entries)if(entry.full) {
            Scene3D settings;
            append(d.errors,apply_scene3d_settings(staging.world,settings,nullptr,entry.copy));
            d.gather_entity(staging.watches,entry.copy);
            if(models)for(const auto* name:{"ModelRenderer3D","SkinnedMeshRenderer","ModelMaterials3D","Animator3D","AnimatorAsset3D"})
                if(staging.world.components().has(entry.copy,name))entry.changed.insert(name);
            if(animation&&entry.copy.has<Animator3D>())entry.changed.insert("Animator3D");
            if(entry.body&&entry.copy.has<Collider3D>())entry.changed.insert("Collider3D");
        }
        size_t dependencies=active.watches.size();
        for(const auto& [file,stamp]:staging.watches)if(!active.watches.contains(file))++dependencies;
        require(dependencies<=d.options.max_dependencies,"live edit exceeds dependency limit; reload to prune old dependencies");
        if(!d.errors.empty())return false;

        View3D new_view;auto new_options=d.options.view;
        i32 priority=std::numeric_limits<i32>::min();EcsId selected=0;
        active.world.raw().each([&](flecs::entity entity,const SceneEnvironment3D& original){
            const auto it=entries.find(entity.id());
            const auto* c=it!=entries.end()&&it->second.full?it->second.copy.get<SceneEnvironment3D>():&original;
            if(c->enabled&&(!selected||c->priority>priority||(c->priority==priority&&entity.id()<selected))) {
                priority=c->priority;selected=entity.id();new_options=c->view;
            }
        });
        if(!same_options(new_options,active.options)) {
            new_view=d.renderer.create_view(d.options.viewport,new_options);
            require(bool(new_view),"edited scene view creation failed");
        }
        std::vector<WorldPhysicsState3D::BodyEdit> bodies;
        for(auto& [id,entry]:entries)if(entry.body&&entry.copy.has<Collider3D>()) {
            bool linear=true,angular=true;
            for(const auto& edit:edits)if(edit.entity.id()==id&&edit.component=="RigidBody3D") {
                linear&=edit.field!="linear_velocity";angular&=edit.field!="angular_velocity";
            }
            bodies.push_back({entry.source.raw(),entry.copy.raw(),!entry.move,linear,angular});
        }
        const auto commit=[&] {
            // Suppress the bulk native copy's observer; publish the ordinary
            // field events after all data and bindings have been committed.
            for(auto& [id,entry]:entries)for(const auto& name:entry.changed) {
                const auto* descriptor=active.world.components().find(name);
                const auto* info=ecs_get_type_info(active.world.raw().c_ptr(),descriptor->id);
                if(descriptor->kind==ComponentKind::Native&&info&&info->size)
                    active.world.events().suppress_next_observer(EcsEventKind::ComponentChanged,id,descriptor->id);
                staging.world.components().copy_component(entry.copy,name,active.world.components(),entry.source);
            }
            active.watches.merge(staging.watches);
            if(new_view){active.view=std::move(new_view);active.options=new_options;}
        };
        std::string error;
        if(active.bodies)require(active.bodies->replace_bodies(bodies,commit,&error),error);
        else commit();
        for(const auto& [id,entry]:entries)for(const auto& name:entry.changed) {
            const bool patched=std::ranges::any_of(edits,[&](const auto& edit){return edit.entity.id()==id&&edit.component==name;});
            if(!patched) {
                const auto* descriptor=active.world.components().find(name);
                active.world.events().emit_component(EcsEventKind::ComponentChanged,EcsEventSource::KinApi,entry.source,
                    descriptor->id,descriptor->name);
            }
        }
        for(const auto& edit:edits) {
            const auto* descriptor=active.world.components().find(edit.component);
            active.world.events().emit_component(EcsEventKind::ComponentChanged,EcsEventSource::KinApi,edit.entity,
                descriptor->id,descriptor->name,edit.field);
        }
        return true;
    } catch(const std::exception& error){d.errors.push_back("live scene edit: "+std::string(error.what()));}
    return false;
}
SceneEntity3D SceneSession3D::handle(EcsEntity entity) {
    auto& d=*_data;
    if(!d.current || entity.raw().world().c_ptr()!=d.current->world.raw().c_ptr() || !entity)return {};
    const auto value=d.current->world.entities().handle(entity);
    const auto owner=d.current->owners.find(entity.id());
    if(owner!=d.current->owners.end()&&d.current->spawned.at(owner->second).inactive)return {};
    return value.id?SceneEntity3D{d.identity,d.generation,value,owner==d.current->owners.end()?0:owner->second}:SceneEntity3D{};
}
EcsEntity SceneSession3D::resolve(SceneEntity3D entity) {
    auto& d=*_data;
    if(!d.current || entity.session!=d.identity || entity.generation!=d.generation)return {};
    const auto owner=d.current->owners.find(entity.entity.id);
    if(owner!=d.current->owners.end()) {
        if(entity.instance!=owner->second||d.current->spawned.at(owner->second).inactive)return {};
    } else if(entity.instance)return {};
    return entity.resolve(d.current->world);
}
bool SceneSession3D::alive(ScenePrefab3D prefab) {
    auto& d=*_data;
    if(!d.current || prefab.session!=d.identity || prefab.generation!=d.generation)return false;
    const auto it=d.current->spawned.find(prefab.id);
    return it!=d.current->spawned.end() && it->second.handle.root==prefab.root && bool(resolve(prefab.root));
}
SceneEntity3D SceneSession3D::entity(ScenePrefab3D prefab,std::string_view local_id) {
    if(!alive(prefab))return {};
    const auto& record=_data->current->spawned.at(prefab.id);
    const auto it=record.local.find(std::string(local_id));
    if(it==record.local.end())return {};
    SceneEntity3D entity{prefab.session,prefab.generation,it->second,prefab.id};
    return resolve(entity)?entity:SceneEntity3D{};
}
std::size_t SceneSession3D::runtime_prefabs() const {return active()?std::ranges::count_if(_data->current->spawned,[](const auto& item){return !item.second.inactive;}):0;}
std::size_t SceneSession3D::runtime_entities() const {
    size_t count=0;if(active())for(const auto& [id,record]:_data->current->spawned)if(!record.inactive)count+=record.members.size();return count;
}
std::size_t SceneSession3D::reserved_entities() const {return active()?_data->current->owners.size():0;}
bool SceneSession3D::configuration_error(std::string error){_data->errors={std::move(error)};return false;}

bool SceneSession3D::reconfigure_native(SceneEntity3D handle,ComponentId component,const std::function<void(EcsEntity)>& change) {
    auto& d=*_data;d.errors.clear();
    if(!d.gameplay_boundary())return false;
    auto original=resolve(handle);
    const auto* descriptor=d.current->world.components().find(component);
    if(!original || !descriptor || descriptor->kind!=ComponentKind::Native || !d.current->world.components().has(original,component))
        return configuration_error("typed configuration requires an existing reflected native component; use native APIs for runtime-only state");
    std::vector<SceneFieldEdit3D> edits;
    try {
        {
            BoolScope busy(d.editing);
            EcsWorld scratch;register_builtin_editor_components(scratch);
            if(d.options.register_components)d.options.register_components(scratch);
            auto copy=scratch.entity();
            if(!d.current->world.components().copy_component(original,descriptor->name,scratch.components(),copy))
                throw std::runtime_error(scratch.components().last_error());
            const auto before=scratch.components().snapshot(copy,descriptor->name);
            change(copy);
            const auto after=scratch.components().snapshot(copy,descriptor->name);
            if(!before||!after)throw std::runtime_error("configuration callback removed its component");
            for(size_t i=0;i<after->fields.size();++i)if(after->fields[i].value!=before->fields[i].value)
                edits.push_back({original,descriptor->name,after->fields[i].name,after->fields[i].value});
        }
        return edits.empty() || edit(edits);
    } catch(const std::exception& error){return configuration_error("typed configuration: "+std::string(error.what()));}
}

ScenePrefab3D SceneSession3D::spawn(const std::filesystem::path& path,SceneSpawn3D options) {
    auto& d=*_data;d.errors.clear();
    if(!d.gameplay_boundary())return {};
    return spawn_now(path,std::move(options));
}
ScenePrefab3D SceneSession3D::spawn_now(const std::filesystem::path& path,SceneSpawn3D options,const PrefabAsset* source,u64 prepared) {
    auto& d=*_data;d.errors.clear();
    auto& state=*d.current;BoolScope busy(d.editing);
    PrefabInstance instance;
    std::vector<EcsId> ids;
    const auto rollback=[&] {
        // Each member can have been detached by a failing callback. Resolve the
        // original ECS handle before destruction; never follow a recycled ID.
        for(auto it=instance.entities.rbegin();it!=instance.entities.rend();++it)
            if(*it)state.world.entities().destroy(*it);
        if(state.bodies&&!ids.empty())state.bodies->sync_entities(ids);
        d.assets.trim();d.collision_assets.trim();
    };
    try {
        const auto require=[](bool valid,const std::string& why){if(!valid)throw std::runtime_error(why);};
        require(source||(!path.empty()&&path.native().size()<=4096),"prefab path is empty or exceeds 4096 bytes");
        require(state.spawned.size()<d.options.max_runtime_prefabs,"runtime prefab count budget exceeded");
        EcsEntity parent;
        if(options.parent){parent=resolve(options.parent);require(bool(parent),"spawn parent handle is stale or foreign");}
        if(parent)if(auto owner=state.owners.find(parent.id());owner!=state.owners.end())
            require(!state.spawned.at(owner->second).pool,"pooled objects have a fixed hierarchy; runtime children are not supported");
        const auto capacity=std::min(d.options.max_spawn_entities,d.options.max_runtime_entities-state.owners.size());
        require(capacity>0,"runtime entity budget exhausted");
        PrefabAssetLoadOptions limits{.content_root=d.assets.root(),.max_source_bytes=d.options.limits.max_source_bytes,
            .max_entities=capacity,.max_depth=d.options.limits.max_prefab_depth,.max_sources=d.options.limits.max_prefab_sources};
        PrefabLoadResult loaded;
        if(!source){loaded=load_prefab_asset(d.assets.root()/path,state.world.components(),limits);if(!loaded.ok()){append(d.errors,loaded.diagnostics);return {};}source=&*loaded.prefab;}
        const auto& recipe=resolved_prefab_asset(*source);
        require(recipe.entities.size()<=capacity,"spawn exceeds runtime/per-spawn entity budget");
        if(d.options.limits.max_entities)
            require(state.world.entities().all().size()+recipe.entities.size()<=d.options.limits.max_entities,"spawn exceeds scene entity limit");
        instance=instantiate_prefab(state.world,*source,PrefabSpawn{.parent=parent},state.world.components());
        if(!instance.ok()){append(d.errors,instance.diagnostics);return {};}
        std::map<EcsId,EcsId> parents;
        for(auto e:instance.entities){ids.push_back(e.id());parents[e.id()]=e.parent().id();}
        std::ranges::sort(ids);
        if(prepared) {
            const auto& reference=state.spawned.at(state.prepared.at(prepared).reference);
            for(const auto& entity:recipe.entities)for(const auto& component:entity.components)
                d.copy_bound(state,state.world.entities().resolve(reference.local.at(entity.id)),instance.by_authored_id.at(entity.id),component.name,
                    reference,[&](const auto& local){return instance.by_authored_id.at(local);});
            if(options.scale)require(instance.root.has<Transform3D>()&&*options.scale==instance.root.get<Transform3D>()->scale,"preloaded spawn requires its prepared scale");
        }
        if(options.position||options.rotation||options.scale) {
            auto& transform=instance.root.ensure<Transform3D>();
            if(options.position)transform.position=*options.position;
            if(options.rotation)transform.rotation=*options.rotation;
            if(options.scale)transform.scale=*options.scale;
        }
        if(options.configure){SceneSpawnContext3D context{state.world,instance.root,instance.by_authored_id};options.configure(context);}
        if(prepared) {
            const auto& reference=state.spawned.at(state.prepared.at(prepared).reference);
            for(const auto& [local,e]:instance.by_authored_id)d.fixed_assets(state.world.entities().resolve(reference.local.at(local)),e);
        }
        for(auto e:instance.entities) {
            require(bool(e),"spawn configuration destroyed a prefab entity");
            require(e.parent().id()==parents.at(e.id()),"spawn configuration reparented a prefab entity");
            e.raw().children([&](flecs::entity child){require(std::ranges::binary_search(ids,EcsId(child.id())),"spawn configuration created an unmanaged child");});
            require(!e.has<SceneCamera3D>()&&!e.has<SceneEnvironment3D>(),"runtime object prefabs cannot replace scene camera/environment settings");
            if(const auto* t=e.get<Transform3D>()) {
                require(detail::field_value_matches_kind(t->position,ComponentFieldKind::Vec3f)&&
                    detail::field_value_matches_kind(t->rotation,ComponentFieldKind::Quatf)&&
                    detail::field_value_matches_kind(t->scale,ComponentFieldKind::Vec3f)&&t->scale.x>0&&t->scale.y>0&&t->scale.z>0,"spawn has an invalid transform");
            }
            require(!(e.has<Collider3D>()||e.has<CharacterController3D>()||e.has<Ragdoll3D>())||e.has<Transform3D>(),"spawned physics object requires Transform3D");
            if(const auto* body=e.get<RigidBody3D>()) {
                const float values[]={body->mass,body->density,body->friction,body->restitution,body->linear_damping,body->angular_damping,body->gravity_factor};
                require(std::ranges::all_of(values,[](float v){return std::isfinite(v);})&&body->mass>=0&&body->density>=0&&
                    body->friction>=0&&body->restitution>=0&&body->restitution<=1&&body->linear_damping>=0&&body->angular_damping>=0,
                    "spawn has invalid rigid-body settings");
            }
        }
        append(d.errors,validate_particles3d(state.world,d.options.particle_limits));
        if(!d.errors.empty()){rollback();return {};}
        if(!prepared) {
            append(d.errors,resolve_render3d_assets(state.world,d.assets,d.options.particle_limits,ids));
            if(state.bodies)append(d.errors,resolve_physics3d_assets(state.world,d.collision_assets,ids));
        }
        std::map<std::filesystem::path,SourceStamp> dependencies;
        if(!prepared) {
            for(const auto& file:source->source_files)d.watch(dependencies,file);
            for(auto e:instance.entities)d.gather_entity(dependencies,e);
        }
        auto dependency_count=state.watches.size();
        for(const auto& [file,stamp]:dependencies)if(!state.watches.contains(file))++dependency_count;
        require(dependency_count<=d.options.max_dependencies,"spawn exceeds dependency budget");
        if(!d.errors.empty()){rollback();return {};}
        if(state.bodies) {
            state.bodies->sync_entities(ids);
            for(auto e:instance.entities) {
                if(e.has<Collider3D>()&&!e.has<CharacterController3D>())require(state.bodies->body_of(e.raw()).valid(),"spawned rigid body creation refused");
                if(e.has<CharacterController3D>())require(state.bodies->character_of(e.raw()).valid(),"spawned character creation refused");
                if(e.has<Ragdoll3D>())require(state.bodies->ragdoll_of(e.raw()).valid(),"spawned ragdoll creation refused");
                if(e.has<Rope3D>())require(state.bodies->rope_of(e.raw()).valid(),"spawned rope creation refused");
            }
        }
        const u64 id=d.next_spawn++;
        Data::State::Spawned record;
        record.handle={d.identity,d.generation,id,{d.identity,d.generation,state.world.entities().handle(instance.root),id}};
        for(const auto& [local,e]:instance.by_authored_id) {
            const auto value=state.world.entities().handle(e);
            record.local.emplace(local,value);record.members.push_back(value);
        }
        const auto result=record.handle;
        state.spawned.emplace(id,std::move(record));
        for(auto e:instance.entities){state.owners.emplace(e.id(),id);e.set(SceneLease3D{id,true});}
        state.watches.merge(dependencies);
        return result;
    } catch(const std::exception& error){d.errors.push_back("runtime spawn: "+std::string(error.what()));rollback();return {};}
}

ScenePreparedPrefab3D SceneSession3D::preload(const std::filesystem::path& path) {
    auto& d=*_data;d.errors.clear();if(!d.gameplay_boundary())return {};
    auto& state=*d.current;
    if(state.prepared.size()>=d.options.max_preloaded_prefabs){configuration_error("preloaded prefab budget exceeded");return {};}
    if(path.empty()||path.native().size()>4096){configuration_error("invalid prefab path");return {};}
    auto loaded=load_prefab_asset(d.assets.root()/path,state.world.components(),{
        .content_root=d.assets.root(),.max_source_bytes=d.options.limits.max_source_bytes,
        .max_entities=d.options.max_spawn_entities,.max_depth=d.options.limits.max_prefab_depth,.max_sources=d.options.limits.max_prefab_sources});
    if(!loaded.ok()){append(d.errors,loaded.diagnostics);return {};}
    const auto reference=spawn_now({}, {},&*loaded.prefab);
    if(!reference)return {};
    try {
        auto& record=state.spawned.at(reference.id);d.idle(state,record);record.reference=true;
        const auto id=d.next_resource++;
        state.prepared.emplace(id,Data::State::Prepared{std::move(*loaded.prefab),reference.id});
        return {d.identity,d.generation,id};
    } catch(const std::exception& error){
        auto& record=state.spawned.at(reference.id);record.reference=false;record.inactive=false;despawn_now(reference);
        configuration_error("prefab preload: "+std::string(error.what()));return {};
    }
}
ScenePrefab3D SceneSession3D::spawn(ScenePreparedPrefab3D prefab,SceneSpawn3D options) {
    auto& d=*_data;d.errors.clear();if(!d.gameplay_boundary())return {};return spawn_prepared(prefab,std::move(options));
}
ScenePrefab3D SceneSession3D::spawn_prepared(ScenePreparedPrefab3D prefab,SceneSpawn3D options) {
    auto& d=*_data;d.errors.clear();
    if(prefab.session!=d.identity||prefab.generation!=d.generation||!d.current->prepared.contains(prefab.id)) {
        configuration_error("preloaded prefab is stale or foreign");return {};
    }
    return spawn_now({},std::move(options),&d.current->prepared.at(prefab.id).recipe,prefab.id);
}
bool SceneSession3D::release(ScenePreparedPrefab3D prefab) {
    auto& d=*_data;d.errors.clear();if(!d.gameplay_boundary())return false;
    auto& state=*d.current;
    if(prefab.session!=d.identity||prefab.generation!=d.generation||!state.prepared.contains(prefab.id))return configuration_error("preloaded prefab is stale or foreign");
    for(const auto& [id,pool]:state.pools)if(pool.prepared==prefab.id)return configuration_error("release pools before their prepared prefab");
    auto& record=state.spawned.at(state.prepared.at(prefab.id).reference);
    record.inactive=false;record.reference=false;
    if(!despawn_now(record.handle)){record.inactive=true;record.reference=true;return false;}
    state.prepared.erase(prefab.id);return true;
}
ScenePrefabPool3D SceneSession3D::create_pool(ScenePreparedPrefab3D prefab,ScenePrefabPoolOptions3D options) {
    auto& d=*_data;d.errors.clear();if(!d.gameplay_boundary())return {};
    auto& state=*d.current;
    if(prefab.session!=d.identity||prefab.generation!=d.generation||!state.prepared.contains(prefab.id)) {
        configuration_error("preloaded prefab is stale or foreign");return {};
    }
    const auto& prepared=state.prepared.at(prefab.id);const auto& reference=state.spawned.at(prepared.reference);
    if(!options.capacity||state.pools.size()>=d.options.max_prefab_pools||
        options.capacity>d.options.max_runtime_prefabs-state.spawned.size()||
        options.capacity>(d.options.max_runtime_entities-state.owners.size())/reference.members.size()) {
        configuration_error("pool capacity exceeds instance/entity/pool budgets (or is zero)");return {};
    }
    for(auto h:reference.members)if(auto e=state.world.entities().resolve(h);
        e.has<CharacterController3D>()||e.has<Ragdoll3D>()||e.has<Rope3D>()) {
        configuration_error("pools currently support ordinary bodies; controllers/ragdolls/ropes require ordinary spawning");return {};
    }
    if(!state.world.entities().resolve(reference.handle.root.entity).has<Transform3D>()) {
        configuration_error("pooled prefab requires a root Transform3D");return {};
    }
    const auto id=d.next_resource++;const ScenePrefabPool3D result{d.identity,d.generation,id};
    auto& pool=state.pools[id];pool.prepared=prefab.id;pool.options=std::move(options);pool.slots.reserve(pool.options.capacity);
    for(size_t i=0;i<pool.options.capacity;++i) {
        auto instance=spawn_prepared(prefab,{.configure=pool.options.reset});
        if(!instance) {
            auto errors=d.errors;release(result);d.errors=std::move(errors);return {};
        }
        auto& record=state.spawned.at(instance.id);record.pool=id;pool.slots.push_back(instance.id);d.idle(state,record);
    }
    return result;
}
ScenePrefabPoolStats3D SceneSession3D::pool_stats(ScenePrefabPool3D handle) const {
    const auto& d=*_data;
    if(!d.current||handle.session!=d.identity||handle.generation!=d.generation||!d.current->pools.contains(handle.id))return {};
    const auto& pool=d.current->pools.at(handle.id);size_t active=0;
    for(auto id:pool.slots)if(!d.current->spawned.at(id).inactive)++active;
    return {pool.slots.size(),active,pool.slots.size()-active,pool.acquired,pool.reused,pool.exhausted};
}
ScenePrefab3D SceneSession3D::spawn(ScenePrefabPool3D pool,SceneSpawn3D options) {
    auto& d=*_data;d.errors.clear();if(!d.gameplay_boundary())return {};return acquire_pool(pool,std::move(options));
}
ScenePrefab3D SceneSession3D::acquire_pool(ScenePrefabPool3D handle,SceneSpawn3D options) {
    auto& d=*_data;d.errors.clear();auto& state=*d.current;
    if(handle.session!=d.identity||handle.generation!=d.generation||!state.pools.contains(handle.id)) {
        configuration_error("prefab pool is stale or foreign");return {};
    }
    auto& pool=state.pools.at(handle.id);
    auto found=std::ranges::find_if(pool.slots,[&](auto id){return state.spawned.at(id).inactive;});
    if(found==pool.slots.end()){++pool.exhausted;configuration_error("prefab pool exhausted; capacity is fixed");return {};}
    auto& record=state.spawned.at(*found);const auto& prepared=state.prepared.at(pool.prepared);
    BoolScope busy(d.editing);
    try {
        if(options.parent)throw std::runtime_error("pooled objects have a fixed hierarchy; parent placement is unsupported");
        d.fixed_members(state,record,prepared);d.restore(state,record,prepared);
        std::unordered_map<std::string,EcsEntity> local;auto context=d.context(state,record,local);
        auto* root=context.root.get_mut<Transform3D>();
        if(options.scale&&*options.scale!=root->scale)throw std::runtime_error("pool acquisition requires its prepared scale");
        if(options.position)root->position=*options.position;
        if(options.rotation)root->rotation=*options.rotation;
        if(pool.options.reset)pool.options.reset(context);
        if(options.configure)options.configure(context);
        d.fixed_members(state,record,prepared);
        std::vector<EcsId> ids;
        for(auto h:record.members) {
            auto e=state.world.entities().resolve(h);ids.push_back(e.id());
            if(auto* t=e.get<Transform3D>();t&&(!detail::field_value_matches_kind(t->position,ComponentFieldKind::Vec3f)||
                !detail::field_value_matches_kind(t->rotation,ComponentFieldKind::Quatf)||!detail::field_value_matches_kind(t->scale,ComponentFieldKind::Vec3f)||
                t->scale.x<=0||t->scale.y<=0||t->scale.z<=0))throw std::runtime_error("invalid pooled transform");
            if(e.has<CharacterController3D>()||e.has<Ragdoll3D>()||e.has<Rope3D>())throw std::runtime_error("pool reset added unsupported physics components");
        }
        const auto& reference=state.spawned.at(prepared.reference);
        for(const auto& [name,e]:local)d.fixed_assets(state.world.entities().resolve(reference.local.at(name)),e);
        append(d.errors,validate_particles3d(state.world,d.options.particle_limits,ids));
        append(d.errors,validate_particles3d(state.world,d.options.particle_limits));
        if(!d.errors.empty())throw std::runtime_error("invalid pooled particles");
        for(auto h:record.members)state.world.entities().resolve(h).enable();
        if(state.bodies) {
            state.bodies->sync_entities(ids);
            for(auto h:record.members)if(auto e=state.world.entities().resolve(h);e.has<Collider3D>()&&!state.bodies->body_of(e.raw()))
                throw std::runtime_error("pooled rigid body creation refused");
        }
        record.inactive=false;for(auto h:record.members)state.world.entities().resolve(h).set(SceneLease3D{record.handle.id,true});
        ++pool.acquired;if(!pool.used.insert(record.handle.root.entity.id).second)++pool.reused;
        return record.handle;
    } catch(const std::exception& error){
        if(pool.options.release)try {std::unordered_map<std::string,EcsEntity> local;auto context=d.context(state,record,local);pool.options.release(context);}catch(...){d.errors.push_back("pool cleanup callback failed");}
        d.idle(state,record);
        try {d.restore(state,record,prepared);}catch(...){d.errors.push_back("pool state restoration failed; release the pool or reload");}
        d.errors.push_back("pool acquisition: "+std::string(error.what()));return {};
    }
}
bool SceneSession3D::recycle_pool(ScenePrefab3D prefab) {
    auto& d=*_data;auto& state=*d.current;auto& record=state.spawned.at(prefab.id);
    auto& pool=state.pools.at(record.pool);const auto& prepared=state.prepared.at(pool.prepared);
    BoolScope busy(d.editing);
    try {
        d.fixed_members(state,record,prepared);
        if(pool.options.release){std::unordered_map<std::string,EcsEntity> local;auto context=d.context(state,record,local);pool.options.release(context);}
        d.fixed_members(state,record,prepared);d.idle(state,record);d.renew(state,record);d.restore(state,record,prepared);return true;
    } catch(const std::exception& error){return configuration_error("pool return: "+std::string(error.what()));}
}
bool SceneSession3D::release(ScenePrefabPool3D handle) {
    auto& d=*_data;d.errors.clear();if(!d.gameplay_boundary())return false;auto& state=*d.current;
    if(handle.session!=d.identity||handle.generation!=d.generation||!state.pools.contains(handle.id))return configuration_error("prefab pool is stale or foreign");
    auto& pool=state.pools.at(handle.id);
    const auto children=[&] {
        for(auto id:pool.slots)for(auto h:state.spawned.at(id).members)if(auto e=state.world.entities().resolve(h))
            e.raw().children([&](flecs::entity child){
                const auto owner=state.owners.find(child.id());
                if(owner==state.owners.end()||owner->second!=id)throw std::runtime_error("pool release would destroy a foreign child; detach it first");
            });
    };
    BoolScope busy(d.editing);
    try {
        children();
        if(pool.options.release)for(auto id:pool.slots)if(auto& record=state.spawned.at(id);!record.inactive) {
            if(std::ranges::all_of(record.members,[&](auto h){return bool(state.world.entities().resolve(h));})) {
                std::unordered_map<std::string,EcsEntity> local;auto context=d.context(state,record,local);pool.options.release(context);
            }
        }
        children();
    }
    catch(const std::exception& error){return configuration_error(error.what());}
    for(auto id:pool.slots) {
        auto& record=state.spawned.at(id);record.pool=0;record.inactive=false;
        if(!despawn_now(record.handle))return false;
    }
    state.pools.erase(handle.id);return true;
}

bool SceneSession3D::despawn(ScenePrefab3D prefab) {
    auto& d=*_data;d.errors.clear();
    if(!d.gameplay_boundary())return false;
    return despawn_now(prefab);
}
bool SceneSession3D::despawn_now(ScenePrefab3D prefab) {
    auto& d=*_data;d.errors.clear();
    auto& state=*d.current;
    const auto first=state.spawned.find(prefab.id);
    if(prefab.session!=d.identity||prefab.generation!=d.generation||first==state.spawned.end()||first->second.handle.root!=prefab.root)
        return configuration_error("despawn requires an owned runtime prefab handle from the current scene");
    if(first->second.reference||first->second.inactive)return configuration_error("reserved objects are released through their preload/pool owner");
    if(first->second.pool)return recycle_pool(prefab);
    BoolScope busy(d.editing);
    try {
        std::set<u64> records;
        std::map<EcsId,EcsEntityHandle> members;
        std::vector<EcsEntityHandle> queue;
        const auto add_record=[&](u64 id) {
            if(state.spawned.at(id).pool||state.spawned.at(id).reference)
                throw std::runtime_error("despawn would destroy a pool/reference member; detach it explicitly");
            if(!records.insert(id).second)return;
            for(auto h:state.spawned.at(id).members)if(members.emplace(h.id,h).second)queue.push_back(h);
        };
        add_record(prefab.id);
        for(size_t i=0;i<queue.size();++i)if(auto e=state.world.entities().resolve(queue[i]))
            e.raw().children([&](flecs::entity child) {
                const auto owner=state.owners.find(child.id());
                if(owner==state.owners.end())throw std::runtime_error("despawn would destroy an authored/unmanaged child; detach it explicitly");
                add_record(owner->second);
            });
        std::vector<EcsId> ids;ids.reserve(members.size());
        for(const auto& [id,h]:members)ids.push_back(id);
        for(auto it=members.rbegin();it!=members.rend();++it)if(auto e=state.world.entities().resolve(it->second))state.world.entities().destroy(e);
        if(state.bodies)state.bodies->sync_entities(ids);
        for(auto id:ids)state.owners.erase(id);
        for(auto id:records)state.spawned.erase(id);
        // Release queued draws of removed objects immediately. The next collect
        // rebuilds surviving draws and keeps their ECS-owned asset handles.
        state.scene.clear();
        state.rendering->clear_collection_cache();
        d.assets.trim();d.collision_assets.trim();return true;
    } catch(const std::exception& error){return configuration_error("runtime despawn: "+std::string(error.what()));}
}

SceneQueries3D SceneSession3D::queries()const {return active()?SceneQueries3D{_data->current->commands}:SceneQueries3D{};}
SceneCommands3D SceneSession3D::commands() const {
    return _data->current?SceneCommands3D{_data->current->commands}:SceneCommands3D{};
}
void SceneSession3D::drain_commands(bool systems_succeeded) {
    auto& d=*_data;
    auto& queue=*d.current->commands;
    {std::lock_guard lock(queue.mutex);queue.ready.swap(queue.pending);}
    auto& work=queue.ready;
    // If an unexpected native exception escapes, don't carry partially drained
    // vectors (or executable requests) into the next step.
    struct Cleanup {
        decltype(work) requests;
        ~Cleanup() {
            for(auto& op:requests)if(op)op->cancel("scene lifecycle execution was interrupted");
            requests.clear();
        }
    } cleanup{work};
    for(auto& op:work) {
        if(!systems_succeeded||d.pending) {
            op->cancel(systems_succeeded?
                "scene replacement is prepared; cancel it or activate before stepping commands":"scene systems failed before lifecycle execution");
            op.reset();
            continue;
        }
        ScenePrefab3D prefab;
        bool ok;
        if(op->destroy) {
            auto target=op->prefab;
            if(op->source.status()!=SceneRequestStatus3D::Invalid)target=op->source.prefab();
            ok=despawn_now(target);
        } else {
            if(op->pool)prefab=acquire_pool(op->pool,std::move(op->options));
            else if(op->prepared)prefab=spawn_prepared(op->prepared,std::move(op->options));
            else prefab=spawn_now(op->path,std::move(op->options));
            ok=bool(prefab);
        }
        std::string error;
        if(!ok)for(const auto& message:d.errors){if(!error.empty())error+='\n';error+=message;}
        op->result->finish(ok?SceneRequestStatus3D::Succeeded:SceneRequestStatus3D::Failed,prefab,error);
        d.errors.clear();
        // Release constructor captures and capacity before the next request.
        op.reset();
    }
}

bool SceneSession3D::step(f32 dt) {
    if(_data->editing||_data->in_frame)return configuration_error("recursive scene step during a callback");
    BoolScope frame(_data->in_frame);
    auto& d = *_data; d.errors.clear();
    if (!d.current || !std::isfinite(dt) || dt <= 0) { d.errors = {"scene step requires an active scene and finite positive dt"}; return false; }
    try {
        auto& state = *d.current;
        SystemPhaseHooks hooks;
        hooks.after=[this,dt](SystemPhase phase,std::string& error) {
            auto& d=*_data;auto& state=*d.current;
            const auto validate=[&](std::string& error) {
                append(d.errors,validate_particles3d(state.world,d.options.particle_limits));
                for(const auto& message:d.errors){if(!error.empty())error+='\n';error+=message;}
                return d.errors.empty();
            };
            switch(phase) {
            case SystemPhase::Physics:
                drain_commands(true);
                if(!validate(error))return false;
                if(state.bodies)state.bodies->step(dt);
                state.rendering->propagate_transforms();
                break;
            case SystemPhase::PostPhysics:
                // Contact-driven destruction finishes before animation/drawing.
                // Bodies spawned here first simulate on the following step.
                drain_commands(true);
                break;
            case SystemPhase::Animation:
                drain_commands(true);
                if(!validate(error))return false;
                state.rendering->animate(dt);state.rendering->advance_time_of_day(dt);state.rendering->advance_weather(dt);
                state.rendering->propagate_transforms();state.rendering->simulate_particles(dt);
                break;
            case SystemPhase::Editor:
                // Late-phase requests still commit before collection; native
                // physics/animation/particles are never advanced a second time.
                drain_commands(true);
                if(!validate(error))return false;
                state.rendering->propagate_transforms();
                break;
            default:break;
            }
            return true;
        };
        if(!state.world.run_frame(dt,hooks)) {
            drain_commands(false);d.errors.push_back(state.world.systems().last_error());return false;
        }
        return true;
    } catch (const std::exception& error) { drain_commands(false);d.errors.push_back("scene step: " + std::string(error.what())); return false; }
}
bool SceneSession3D::collect() {
    if(_data->editing||_data->in_frame)return configuration_error("recursive scene collection during a callback");
    BoolScope frame(_data->in_frame);
    auto& d = *_data; d.errors.clear();
    if (!d.current) { d.errors = {"no active scene to collect"}; return false; }
    try { return d.collect(*d.current); }
    catch (const std::exception& error) { d.errors.push_back("scene collection: " + std::string(error.what())); return false; }
}
bool SceneSession3D::render() {
    if (!collect()) return false;
    if (_data->renderer.render(_data->current->view, _data->current->scene)) return true;
    _data->errors = {"scene render failed"}; return false;
}
bool SceneSession3D::resize(Vec2i viewport) {
    if(_data->editing||_data->in_frame)return configuration_error("scene resize cannot run inside a callback");
    auto& d = *_data; d.errors.clear();
    if (viewport.x <= 0 || viewport.y <= 0) { d.errors = {"scene viewport must be positive"}; return false; }
    if (d.pending) { d.errors = {"cancel or activate the prepared scene before resizing"}; return false; }
    const auto previous = d.options.viewport; d.options.viewport = viewport;
    if (d.current && !collect()) { d.options.viewport = previous; return false; }
    return true;
}
bool SceneSession3D::active() const { return bool(_data->current); }
bool SceneSession3D::prepared() const { return bool(_data->pending); }
u64 SceneSession3D::generation() const { return _data->generation; }
EcsWorld* SceneSession3D::world() { return active() ? &_data->current->world : nullptr; }
EcsEntity SceneSession3D::entity(std::string_view id) { return active() ? world()->entities().find_by_authored_id(id) : EcsEntity{}; }
WorldRenderState3D* SceneSession3D::rendering() { return active() ? _data->current->rendering.get() : nullptr; }
World3D* SceneSession3D::physics() { return active() ? _data->current->physics.get() : nullptr; }
WorldPhysicsState3D* SceneSession3D::bodies() { return active() ? _data->current->bodies.get() : nullptr; }
PhysicsAssets3D& SceneSession3D::physics_assets() { return _data->collision_assets; }
const SceneInstance* SceneSession3D::instance() const { return active() ? &_data->current->asset.instance() : nullptr; }
Scene3D* SceneSession3D::scene() { return active() ? &_data->current->scene : nullptr; }
const View3D* SceneSession3D::view() const { return active() ? &_data->current->view : nullptr; }
const std::vector<std::string>& SceneSession3D::diagnostics() const { return _data->errors; }

} // namespace kin
