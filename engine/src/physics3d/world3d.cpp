#include <kin/physics3d/world3d.hpp>

#include "jolt_runtime.hpp"

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorder.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace kin {

namespace {

using physics3d_detail::from_jolt;
using physics3d_detail::to_jolt;

constexpr u32 tree_static = 0;
constexpr u32 tree_moving = 1;
constexpr u32 tree_count = 2;

bool finite(Vec3f v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(Quatf q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

// Layer 0 is the static tree; everything else shares the moving one.
class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return tree_count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(layer == layers3d::Static ? tree_static : tree_moving);
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "layer"; }
#endif
};

// Every object layer is tested against both trees: which pairs collide is
// the layer table's business, narrowed again in the narrow phase's pair filter.
class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    explicit ObjectVsBroadPhase(const std::array<u32, 32>& table) : _table(table) {}
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer tree) const override {
        if (layer >= 32) {
            return false;
        }
        const u32 mask = _table[layer];
        if (tree.GetValue() == tree_static) {
            return (mask & layer_bit(layers3d::Static)) != 0;
        }
        return (mask & ~layer_bit(layers3d::Static)) != 0;
    }

private:
    const std::array<u32, 32>& _table;
};

class ObjectVsObject final : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectVsObject(const std::array<u32, 32>& table) : _table(table) {}
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a < 32 && b < 32 && (_table[a] & layer_bit(static_cast<u8>(b))) != 0;
    }

private:
    const std::array<u32, 32>& _table;
};

// Scratch memory: a block taken once, with the heap behind it when a step needs
// more (Jolt's own allocator would abort).
class TempMemory final : public JPH::TempAllocator {
public:
    explicit TempMemory(std::size_t bytes) : _block(bytes) {}
    void* Allocate(JPH::uint size) override {
        if (_block.CanAllocate(size)) {
            return _block.Allocate(size);
        }
        return _heap.Allocate(size);
    }
    void Free(void* address, JPH::uint size) override {
        if (address != nullptr && !_block.OwnsMemory(address)) {
            _heap.Free(address, size);
            return;
        }
        _block.Free(address, size);
    }

private:
    JPH::TempAllocatorImpl _block;
    JPH::TempAllocatorMalloc _heap;
};

// Jolt's jobs on kin's job system. A job is queued on a worker; the thread that
// steps works on the jobs of its barrier too (JobSystemWithBarrier), so a pool
// that is busy, or has no free background slot, only makes a step slower.
class KinJobs final : public JPH::JobSystemWithBarrier {
public:
    explicit KinJobs(kin::JobSystem& jobs)
        : JPH::JobSystemWithBarrier(JPH::cMaxPhysicsBarriers)
        , _jobs(jobs) {
        _free_list.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs);
    }
    ~KinJobs() override {
        // Queued closures hold a reference to a job: wait for them (the pool
        // must still be alive, as World3DConfig says).
        while (_pending.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }
    }

    int GetMaxConcurrency() const override { return _jobs.worker_count() + 1; }

    JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                             JPH::uint32 dependencies = 0) override {
        JPH::uint32 index;
        for (;;) {
            index = _free_list.ConstructObject(name, color, this, function, dependencies);
            if (index != Jobs::cInvalidObjectIndex) {
                break;
            }
            std::this_thread::yield();
        }
        Job* job = &_free_list.Get(index);
        JPH::JobHandle handle(job);
        if (dependencies == 0) {
            QueueJob(job);
        }
        return handle;
    }

protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        _pending.fetch_add(1, std::memory_order_relaxed);
        _jobs.run([this, job] {
            job->Execute(); // runs once only: the barrier may have run it already
            job->Release();
            _pending.fetch_sub(1, std::memory_order_release);
        });
    }
    void QueueJobs(Job** jobs, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) {
            QueueJob(jobs[i]);
        }
    }
    void FreeJob(Job* job) override { _free_list.DestructObject(job); }

private:
    using Jobs = JPH::FixedSizeFreeList<Job>;
    kin::JobSystem& _jobs;
    Jobs _free_list;
    std::atomic<i32> _pending{0};
};

// FNV-1a 64 over fixed-width little-endian fields.
class Hasher {
public:
    void byte(u8 b) {
        _h ^= b;
        _h *= 0x100000001b3ull;
    }
    void u32v(u32 v) {
        for (int i = 0; i < 4; ++i) {
            byte(static_cast<u8>(v >> (8 * i)));
        }
    }
    void u64v(u64 v) {
        for (int i = 0; i < 8; ++i) {
            byte(static_cast<u8>(v >> (8 * i)));
        }
    }
    void f32v(f32 v) { u32v(std::isnan(v) ? 0x7fc00000u : std::bit_cast<u32>(v)); }
    void vec(Vec3f v) {
        f32v(v.x);
        f32v(v.y);
        f32v(v.z);
    }
    u64 value() const { return _h; }

private:
    u64 _h = 0xcbf29ce484222325ull;
};

// A StateRecorder over a byte vector.
class ByteRecorder final : public JPH::StateRecorder {
public:
    ByteRecorder() = default;
    explicit ByteRecorder(std::span<const u8> bytes) : _bytes(bytes.begin(), bytes.end()) {}

    void WriteBytes(const void* data, std::size_t count) override {
        const auto* p = static_cast<const u8*>(data);
        _bytes.insert(_bytes.end(), p, p + count);
    }
    void ReadBytes(void* data, std::size_t count) override {
        if (_read + count > _bytes.size()) {
            _failed = true;
            std::memset(data, 0, count);
            return;
        }
        std::memcpy(data, _bytes.data() + _read, count);
        _read += count;
    }
    bool IsEOF() const override { return _read >= _bytes.size(); }
    bool IsFailed() const override { return _failed; }

    std::vector<u8>& bytes() { return _bytes; }
    std::size_t read_position() const { return _read; }

private:
    std::vector<u8> _bytes;
    std::size_t _read = 0;
    bool _failed = false;
};

constexpr u32 state_magic = 0x33444B31u; // "1K D3"-ish tag; the content is what is checked

} // namespace

struct World3D::Impl {
    struct Slot {
        JPH::BodyID id;
        u32 generation = 0;
        u64 user_id = 0;
        f32 mass = 0.0f;
        bool alive = false;
    };

    explicit Impl(const World3DConfig& cfg)
        // Jolt's allocator must be registered before any member below allocates.
        : config((physics3d_detail::ensure_jolt_runtime(), cfg))
        , broad_phase()
        , object_vs_broad_phase(table)
        , object_vs_object(table)
        , temp(cfg.temp_memory_bytes) {
        // Everything collides with everything, bar sensors with sensors.
        table.fill(0xFFFFFFFFu);
        table[layers3d::Sensor] &= ~layer_bit(layers3d::Sensor);
        system.Init(cfg.max_bodies, 0, cfg.max_body_pairs, cfg.max_contact_constraints, broad_phase,
                    object_vs_broad_phase, object_vs_object);
        system.SetGravity(to_jolt(cfg.gravity));
        if (cfg.single_threaded) {
            single.reset(new JPH::JobSystemSingleThreaded(JPH::cMaxPhysicsJobs));
            job_system = single.get();
        } else {
            pooled = std::make_unique<KinJobs>(cfg.jobs != nullptr ? *cfg.jobs : default_job_system());
            job_system = pooled.get();
        }
    }

    ~Impl() {
        // Bodies must go before the system does (Jolt frees them with it, but
        // this keeps shapes' references ordered and the slot table honest).
        JPH::BodyInterface& bi = system.GetBodyInterfaceNoLock();
        for (Slot& slot : slots) {
            if (slot.alive) {
                bi.RemoveBody(slot.id);
                bi.DestroyBody(slot.id);
            }
        }
    }

    Slot* find(Body3D body) {
        if (body.index >= slots.size()) {
            return nullptr;
        }
        Slot& slot = slots[body.index];
        return slot.alive && slot.generation == body.generation ? &slot : nullptr;
    }
    const Slot* find(Body3D body) const { return const_cast<Impl*>(this)->find(body); }

    JPH::BodyInterface& bi() { return system.GetBodyInterfaceNoLock(); }
    const JPH::BodyInterface& bi() const { return system.GetBodyInterfaceNoLock(); }

    World3DConfig config;
    std::array<u32, 32> table{};
    BroadPhaseLayers broad_phase;
    ObjectVsBroadPhase object_vs_broad_phase;
    ObjectVsObject object_vs_object;
    JPH::PhysicsSystem system;
    TempMemory temp;
    std::unique_ptr<JPH::JobSystemSingleThreaded> single;
    std::unique_ptr<KinJobs> pooled;
    JPH::JobSystem* job_system = nullptr;
    std::vector<Slot> slots;
    std::vector<u32> free_slots; // last in, first out
    i32 live = 0;
};

World3D::World3D(World3DConfig config)
    : _impl(std::make_unique<Impl>(config)) {}

World3D::~World3D() = default;

bool World3D::cross_platform_deterministic() {
#ifdef JPH_CROSS_PLATFORM_DETERMINISTIC
    return true;
#else
    return false;
#endif
}

const World3DConfig& World3D::config() const { return _impl->config; }

void World3D::set_gravity(Vec3f gravity) {
    if (finite(gravity)) {
        _impl->config.gravity = gravity;
        _impl->system.SetGravity(to_jolt(gravity));
    }
}

Vec3f World3D::gravity() const { return from_jolt(_impl->system.GetGravity()); }

void World3D::step(f32 dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        return;
    }
    _impl->system.Update(dt, std::max(1, _impl->config.collision_steps), &_impl->temp, _impl->job_system);
}

Body3D World3D::create_body(const Body3DDesc& d) {
    Impl& m = *_impl;
    if (!d.shape.valid() || !finite(d.position) || !finite(d.rotation) || !finite(d.linear_velocity)
        || !finite(d.angular_velocity) || !std::isfinite(d.mass) || !std::isfinite(d.density)) {
        return {};
    }
    const Shape3DKind kind = d.shape.kind();
    const bool surface = kind == Shape3DKind::TriangleMesh || kind == Shape3DKind::HeightField;
    if (d.motion == Body3DMotion::Dynamic && surface) {
        return {};
    }
    u8 layer = d.layer;
    if (layer == layers3d::Auto) {
        layer = d.motion == Body3DMotion::Static ? layers3d::Static
              : d.motion == Body3DMotion::Kinematic ? layers3d::Kinematic : layers3d::Dynamic;
    }
    if (layer >= layers3d::Count || (layer == layers3d::Static && d.motion != Body3DMotion::Static)) {
        return {};
    }
    if (m.live >= static_cast<i32>(m.config.max_bodies)) {
        return {};
    }

    const JPH::EMotionType motion = d.motion == Body3DMotion::Static ? JPH::EMotionType::Static
                                  : d.motion == Body3DMotion::Kinematic ? JPH::EMotionType::Kinematic
                                                                        : JPH::EMotionType::Dynamic;
    JPH::BodyCreationSettings settings(d.shape._data->shape.GetPtr(), to_jolt(d.position),
                                       to_jolt(d.rotation.normalized()), motion, JPH::ObjectLayer(layer));
    settings.mLinearVelocity = to_jolt(d.linear_velocity);
    settings.mAngularVelocity = to_jolt(d.angular_velocity);
    settings.mFriction = d.friction;
    settings.mRestitution = d.restitution;
    settings.mLinearDamping = d.linear_damping;
    settings.mAngularDamping = d.angular_damping;
    settings.mGravityFactor = d.gravity_factor;
    settings.mIsSensor = d.sensor;
    settings.mAllowSleeping = d.allow_sleeping;
    settings.mMotionQuality = d.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    f32 mass = 0.0f;
    if (d.motion == Body3DMotion::Dynamic) {
        mass = d.mass > 0.0f ? d.mass : d.density * d.shape.volume();
        if (!(mass > 0.0f)) {
            return {};
        }
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = mass;
    }

    JPH::Body* body = m.bi().CreateBody(settings);
    if (body == nullptr) {
        return {};
    }
    u32 index;
    if (!m.free_slots.empty()) {
        index = m.free_slots.back();
        m.free_slots.pop_back();
    } else {
        index = static_cast<u32>(m.slots.size());
        m.slots.emplace_back();
    }
    Impl::Slot& slot = m.slots[index];
    slot.id = body->GetID();
    slot.user_id = d.user_id;
    slot.mass = mass;
    slot.alive = true;
    const bool wake = d.start_active && d.motion != Body3DMotion::Static;
    m.bi().AddBody(slot.id, wake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    ++m.live;
    return {index, slot.generation};
}

void World3D::destroy_body(Body3D body) {
    Impl& m = *_impl;
    Impl::Slot* slot = m.find(body);
    if (slot == nullptr) {
        return;
    }
    // Jolt does not wake what a removed body held up: wake whatever touches or
    // overlaps its (slightly grown) box, in body-number order so that the order
    // of activation is the same in every run.
    {
        JPH::AABox bounds = m.bi().GetTransformedShape(slot->id).GetWorldSpaceBounds();
        bounds.ExpandBy(JPH::Vec3::sReplicate(0.1f));
        struct Touching final : JPH::CollideShapeBodyCollector {
            void AddHit(const JPH::BodyID& id) override { ids.push_back(id); }
            std::vector<JPH::BodyID> ids;
        } touching;
        m.system.GetBroadPhaseQuery().CollideAABox(bounds, touching);
        std::erase_if(touching.ids, [&](const JPH::BodyID& id) { return id == slot->id; });
        std::ranges::sort(touching.ids, [](const JPH::BodyID& a, const JPH::BodyID& b) {
            return a.GetIndex() < b.GetIndex();
        });
        if (!touching.ids.empty()) {
            m.bi().ActivateBodies(touching.ids.data(), static_cast<int>(touching.ids.size()));
        }
    }
    m.bi().RemoveBody(slot->id);
    m.bi().DestroyBody(slot->id);
    slot->alive = false;
    slot->id = JPH::BodyID();
    ++slot->generation;
    m.free_slots.push_back(body.index);
    --m.live;
}

bool World3D::contains(Body3D body) const { return _impl->find(body) != nullptr; }
i32 World3D::body_count() const { return _impl->live; }
i32 World3D::active_body_count() const {
    return static_cast<i32>(_impl->system.GetNumActiveBodies(JPH::EBodyType::RigidBody));
}

void World3D::set_position(Body3D body, Vec3f position) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(position)) {
        _impl->bi().SetPosition(s->id, to_jolt(position), JPH::EActivation::Activate);
    }
}

void World3D::set_rotation(Body3D body, Quatf rotation) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(rotation)) {
        _impl->bi().SetRotation(s->id, to_jolt(rotation.normalized()), JPH::EActivation::Activate);
    }
}

void World3D::set_linear_velocity(Body3D body, Vec3f velocity) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(velocity)) {
        _impl->bi().SetLinearVelocity(s->id, to_jolt(velocity));
    }
}

void World3D::set_angular_velocity(Body3D body, Vec3f velocity) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(velocity)) {
        _impl->bi().SetAngularVelocity(s->id, to_jolt(velocity));
    }
}

void World3D::move_kinematic(Body3D body, Vec3f position, Quatf rotation, f32 dt) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(position) && finite(rotation) && dt > 0.0f) {
        _impl->bi().MoveKinematic(s->id, to_jolt(position), to_jolt(rotation.normalized()), dt);
    }
}

void World3D::add_force(Body3D body, Vec3f force) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(force)) {
        _impl->bi().AddForce(s->id, to_jolt(force));
    }
}

void World3D::add_force_at(Body3D body, Vec3f force, Vec3f world_point) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(force) && finite(world_point)) {
        _impl->bi().AddForce(s->id, to_jolt(force), to_jolt(world_point));
    }
}

void World3D::add_torque(Body3D body, Vec3f torque) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(torque)) {
        _impl->bi().AddTorque(s->id, to_jolt(torque));
    }
}

void World3D::add_impulse(Body3D body, Vec3f impulse) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(impulse)) {
        _impl->bi().AddImpulse(s->id, to_jolt(impulse));
    }
}

void World3D::add_impulse_at(Body3D body, Vec3f impulse, Vec3f world_point) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(impulse) && finite(world_point)) {
        _impl->bi().AddImpulse(s->id, to_jolt(impulse), to_jolt(world_point));
    }
}

void World3D::add_angular_impulse(Body3D body, Vec3f impulse) {
    if (const auto* s = _impl->find(body); s != nullptr && finite(impulse)) {
        _impl->bi().AddAngularImpulse(s->id, to_jolt(impulse));
    }
}

Vec3f World3D::position(Body3D body) const {
    const auto* s = _impl->find(body);
    return s != nullptr ? from_jolt(_impl->bi().GetPosition(s->id)) : Vec3f{};
}

Quatf World3D::rotation(Body3D body) const {
    const auto* s = _impl->find(body);
    return s != nullptr ? from_jolt(_impl->bi().GetRotation(s->id)) : Quatf{};
}

Vec3f World3D::centre_of_mass_position(Body3D body) const {
    const auto* s = _impl->find(body);
    return s != nullptr ? from_jolt(_impl->bi().GetCenterOfMassPosition(s->id)) : Vec3f{};
}

Vec3f World3D::linear_velocity(Body3D body) const {
    const auto* s = _impl->find(body);
    return s != nullptr ? from_jolt(_impl->bi().GetLinearVelocity(s->id)) : Vec3f{};
}

Vec3f World3D::angular_velocity(Body3D body) const {
    const auto* s = _impl->find(body);
    return s != nullptr ? from_jolt(_impl->bi().GetAngularVelocity(s->id)) : Vec3f{};
}

Body3DMotion World3D::motion(Body3D body) const {
    const auto* s = _impl->find(body);
    if (s == nullptr) {
        return Body3DMotion::Static;
    }
    switch (_impl->bi().GetMotionType(s->id)) {
    case JPH::EMotionType::Kinematic: return Body3DMotion::Kinematic;
    case JPH::EMotionType::Dynamic: return Body3DMotion::Dynamic;
    default: return Body3DMotion::Static;
    }
}

u8 World3D::layer(Body3D body) const {
    const auto* s = _impl->find(body);
    return s != nullptr ? static_cast<u8>(_impl->bi().GetObjectLayer(s->id)) : u8{0};
}

u64 World3D::user_id(Body3D body) const {
    const auto* s = _impl->find(body);
    return s != nullptr ? s->user_id : 0;
}

void World3D::set_user_id(Body3D body, u64 user_id) {
    if (auto* s = _impl->find(body)) {
        s->user_id = user_id;
    }
}

f32 World3D::mass(Body3D body) const {
    const auto* s = _impl->find(body);
    return s != nullptr ? s->mass : 0.0f;
}

Shape3D World3D::shape(Body3D body) const {
    Shape3D out;
    const auto* s = _impl->find(body);
    if (s == nullptr) {
        return out;
    }
    // The shape a body was made from is not kept apart from Jolt's; wrap it.
    auto data = std::make_shared<physics3d_detail::ShapeData>();
    data->shape = _impl->bi().GetShape(s->id);
    switch (data->shape->GetSubType()) {
    case JPH::EShapeSubType::Box: data->kind = Shape3DKind::Box; break;
    case JPH::EShapeSubType::Sphere: data->kind = Shape3DKind::Sphere; break;
    case JPH::EShapeSubType::Capsule: data->kind = Shape3DKind::Capsule; break;
    case JPH::EShapeSubType::Cylinder: data->kind = Shape3DKind::Cylinder; break;
    case JPH::EShapeSubType::ConvexHull: data->kind = Shape3DKind::ConvexHull; break;
    case JPH::EShapeSubType::Mesh: data->kind = Shape3DKind::TriangleMesh; break;
    case JPH::EShapeSubType::HeightField: data->kind = Shape3DKind::HeightField; break;
    default: data->kind = Shape3DKind::Compound; break;
    }
    out._data = std::move(data);
    return out;
}

bool World3D::is_active(Body3D body) const {
    const auto* s = _impl->find(body);
    return s != nullptr && _impl->bi().IsActive(s->id);
}

void World3D::activate(Body3D body) {
    if (const auto* s = _impl->find(body)) {
        _impl->bi().ActivateBody(s->id);
    }
}

void World3D::deactivate(Body3D body) {
    if (const auto* s = _impl->find(body)) {
        _impl->bi().DeactivateBody(s->id);
    }
}

void World3D::set_layers_collide(u8 a, u8 b, bool collide) {
    if (a >= 32 || b >= 32) {
        return;
    }
    auto& table = _impl->table;
    if (collide) {
        table[a] |= layer_bit(b);
        table[b] |= layer_bit(a);
    } else {
        table[a] &= ~layer_bit(b);
        table[b] &= ~layer_bit(a);
    }
}

bool World3D::layers_collide(u8 a, u8 b) const {
    return a < 32 && b < 32 && (_impl->table[a] & layer_bit(b)) != 0;
}

Layer3DMask World3D::collision_mask(u8 layer) const {
    return layer < 32 ? _impl->table[layer] : 0;
}

void World3D::optimize_broad_phase() { _impl->system.OptimizeBroadPhase(); }

u64 World3D::state_hash() const {
    const Impl& m = *_impl;
    Hasher h;
    h.u32v(static_cast<u32>(m.live));
    for (u32 i = 0; i < m.slots.size(); ++i) {
        const Impl::Slot& slot = m.slots[i];
        if (!slot.alive) {
            continue;
        }
        const JPH::BodyID id = slot.id;
        const JPH::BodyInterface& bi = m.bi();
        h.u32v(i);
        h.u32v(slot.generation);
        h.u64v(slot.user_id);
        h.byte(static_cast<u8>(bi.GetMotionType(id)));
        h.byte(static_cast<u8>(bi.GetObjectLayer(id)));
        h.byte(bi.IsActive(id) ? 1 : 0);
        h.vec(from_jolt(bi.GetPosition(id)));
        const Quatf q = from_jolt(bi.GetRotation(id));
        h.f32v(q.x);
        h.f32v(q.y);
        h.f32v(q.z);
        h.f32v(q.w);
        h.vec(from_jolt(bi.GetLinearVelocity(id)));
        h.vec(from_jolt(bi.GetAngularVelocity(id)));
    }
    return h.value();
}

std::vector<u8> World3D::save_state() const {
    const Impl& m = *_impl;
    ByteRecorder out;
    // The slot table first: a restore is only valid for the same set of bodies.
    const u32 magic = state_magic;
    const u32 count = static_cast<u32>(m.slots.size());
    out.WriteBytes(&magic, sizeof magic);
    out.WriteBytes(&count, sizeof count);
    for (const Impl::Slot& slot : m.slots) {
        const u32 generation = slot.generation;
        const u32 alive = slot.alive ? 1u : 0u;
        out.WriteBytes(&generation, sizeof generation);
        out.WriteBytes(&alive, sizeof alive);
        out.WriteBytes(&slot.user_id, sizeof slot.user_id);
    }
    m.system.SaveState(out);
    return std::move(out.bytes());
}

bool World3D::restore_state(std::span<const u8> state) {
    Impl& m = *_impl;
    ByteRecorder in(state);
    u32 magic = 0;
    u32 count = 0;
    in.ReadBytes(&magic, sizeof magic);
    in.ReadBytes(&count, sizeof count);
    if (in.IsFailed() || magic != state_magic || count != m.slots.size()) {
        return false;
    }
    std::vector<u64> user_ids(count);
    for (u32 i = 0; i < count; ++i) {
        u32 generation = 0;
        u32 alive = 0;
        in.ReadBytes(&generation, sizeof generation);
        in.ReadBytes(&alive, sizeof alive);
        in.ReadBytes(&user_ids[i], sizeof(u64));
        if (in.IsFailed() || generation != m.slots[i].generation || (alive != 0) != m.slots[i].alive) {
            return false;
        }
    }
    if (!m.system.RestoreState(in)) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        m.slots[i].user_id = user_ids[i];
    }
    return true;
}

} // namespace kin
