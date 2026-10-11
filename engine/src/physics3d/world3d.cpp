#include <kin/physics3d/world3d.hpp>

#include "character3d_impl.hpp"
#include "jolt_runtime.hpp"

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorder.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
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
    void skip(std::size_t count) { _read += count; }

private:
    std::vector<u8> _bytes;
    std::size_t _read = 0;
    bool _failed = false;
};


constexpr f32 pi_f = 3.14159265358979323846f;
f32 to_radians(f32 degrees) { return degrees * pi_f / 180.0f; }
f32 to_degrees(f32 radians) { return radians * 180.0f / pi_f; }

// Which body a Jolt body is: its number (slot) and the id that slot holds, so that a stale id (a body that was
// destroyed and whose index Jolt reused) is told from the live one.
struct BodyEntry {
    u32 slot = UINT32_MAX;
    JPH::BodyID id;
};

// What a contact callback saw, before it is turned into events after the step (the callbacks arrive from several
// threads in any order: nothing here may depend on that order).
struct RawContact {
    u32 a = 0, b = 0;             // slots, a < b
    u32 sub_a = 0, sub_b = 0;     // sub shape ids as Jolt gave them (swapped with the bodies)
    u8 kind = 0;                  // 0 added, 1 persisted, 2 removed
    bool sensor = false;
    Vec3f point{};
    Vec3f normal{};
    f32 closing = 0.0f;
    u32 points = 0;
    friend bool operator<(const RawContact& x, const RawContact& y) {
        if (x.a != y.a) return x.a < y.a;
        if (x.b != y.b) return x.b < y.b;
        if (x.kind != y.kind) return x.kind < y.kind;
        if (x.sub_a != y.sub_a) return x.sub_a < y.sub_a;
        return x.sub_b < y.sub_b;
    }
};

// Collects Jolt's contact callbacks into a vector under a lock; nothing else happens on Jolt's threads.
class ContactCollector final : public JPH::ContactListener {
public:
    const std::vector<BodyEntry>* entries = nullptr;
    // Pairs that must not collide (bodies joined by a constraint that says so): read only while a step runs.
    const std::unordered_map<u64, i32>* no_collide = nullptr;
    bool persist = false;
    bool events = true;

    JPH::ValidateResult OnContactValidate(const JPH::Body& b1, const JPH::Body& b2, JPH::RVec3Arg,
                                          const JPH::CollideShapeResult&) override {
        if (no_collide != nullptr && !no_collide->empty()) {
            u32 s1 = slot_of(b1.GetID()), s2 = slot_of(b2.GetID());
            if (s1 > s2) {
                std::swap(s1, s2);
            }
            if (no_collide->contains(static_cast<u64>(s1) << 32 | s2)) {
                return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
            }
        }
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void clear() { _raw.clear(); }
    std::vector<RawContact>& raw() { return _raw; }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                        JPH::ContactSettings& settings) override {
        if (events) {
            record(0, b1, b2, m, settings.mIsSensor);
        }
    }
    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                            JPH::ContactSettings& settings) override {
        if (events && persist) {
            record(1, b1, b2, m, settings.mIsSensor);
        }
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        if (!events) {
            return;
        }
        const u32 s1 = slot_of(pair.GetBody1ID());
        const u32 s2 = slot_of(pair.GetBody2ID());
        if (s1 == UINT32_MAX || s2 == UINT32_MAX) {
            return;
        }
        RawContact r;
        r.kind = 2;
        r.a = s1;
        r.b = s2;
        r.sub_a = pair.GetSubShapeID1().GetValue();
        r.sub_b = pair.GetSubShapeID2().GetValue();
        if (r.a > r.b) {
            std::swap(r.a, r.b);
            std::swap(r.sub_a, r.sub_b);
        }
        std::lock_guard lock(_mutex);
        _raw.push_back(r);
    }

private:
    u32 slot_of(const JPH::BodyID& id) const {
        if (entries == nullptr || id.GetIndex() >= entries->size()) {
            return UINT32_MAX;
        }
        const BodyEntry& e = (*entries)[id.GetIndex()];
        return e.id == id ? e.slot : UINT32_MAX;
    }

    void record(u8 kind, const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, bool sensor) {
        RawContact r;
        r.kind = kind;
        r.a = slot_of(b1.GetID());
        r.b = slot_of(b2.GetID());
        if (r.a == UINT32_MAX || r.b == UINT32_MAX) {
            return;
        }
        r.sub_a = m.mSubShapeID1.GetValue();
        r.sub_b = m.mSubShapeID2.GetValue();
        r.sensor = sensor;
        // The mean of the points on both bodies (they are one point unless the shapes overlap).
        JPH::Vec3 sum = JPH::Vec3::sZero();
        const u32 count = static_cast<u32>(m.mRelativeContactPointsOn1.size());
        for (u32 i = 0; i < count; ++i) {
            sum += JPH::Vec3(m.GetWorldSpaceContactPointOn1(i)) + JPH::Vec3(m.GetWorldSpaceContactPointOn2(i));
        }
        const JPH::Vec3 point = count > 0 ? sum / (2.0f * static_cast<f32>(count)) : JPH::Vec3(m.mBaseOffset);
        JPH::Vec3 normal = m.mWorldSpaceNormal; // from body 1 to body 2
        JPH::Vec3 v1 = b1.GetPointVelocity(JPH::RVec3(point));
        JPH::Vec3 v2 = b2.GetPointVelocity(JPH::RVec3(point));
        r.points = count;
        if (r.a > r.b) {
            std::swap(r.a, r.b);
            std::swap(r.sub_a, r.sub_b);
            normal = -normal;
            std::swap(v1, v2);
        }
        r.point = from_jolt(point);
        r.normal = from_jolt(normal);
        r.closing = (v1 - v2).Dot(normal); // a toward b along the normal: positive while they close
        std::lock_guard lock(_mutex);
        _raw.push_back(r);
    }

    std::mutex _mutex;
    std::vector<RawContact> _raw;
};

// Queries: a body is in when its layer is in the mask.
class MaskLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit MaskLayerFilter(Layer3DMask mask) : _mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer < 32 && (_mask >> layer & 1u) != 0; }

private:
    Layer3DMask _mask;
};

class QueryBodyFilter final : public JPH::BodyFilter {
public:
    QueryBodyFilter(JPH::BodyID ignore, bool sensors) : _ignore(ignore), _sensors(sensors) {}
    bool ShouldCollide(const JPH::BodyID& id) const override { return id != _ignore; }
    bool ShouldCollideLocked(const JPH::Body& body) const override { return _sensors || !body.IsSensor(); }

private:
    JPH::BodyID _ignore;
    bool _sensors;
};

constexpr u32 state_magic = 0x33444B32u; // "1K D3"-ish tag; the content is what is checked

} // namespace

struct World3D::Impl {
    struct Slot {
        JPH::BodyID id;
        u32 generation = 0;
        u64 user_id = 0;
        f32 mass = 0.0f;
        bool alive = false;
        // The inner body of a character: made, moved and destroyed by it.
        bool owned = false;
        // Grab and climb: what a hand finds in this body (level setup: not hashed).
        SurfaceMaterial3D material{};
        bool is_hold = false;
        HoldDesc3D hold{};
    };

    // A constraint. The anchors and axis are kept in the bodies' own frames (origin and rotation) so that
    // constraint_info can say where they are now without asking Jolt's constraint-space conventions.
    struct ConstraintSlot {
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        u32 generation = 0;
        Constraint3DKind kind = Constraint3DKind::Fixed;
        Body3D a{};
        Body3D b{};
        Vec3f local_a{}, local_b{}, local_axis{};
        f32 break_force = 0.0f;
        f32 break_torque = 0.0f;
        u64 user_id = 0;
        bool alive = false;
        bool broken = false;
        bool no_collide = false;
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
        collector.entries = &entries;
        collector.no_collide = &no_collide;
        collector.persist = cfg.persist_events;
        collector.events = cfg.contact_events;
        system.SetContactListener(&collector);
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
        for (ConstraintSlot& c : constraint_slots) {
            if (c.alive) {
                system.RemoveConstraint(c.constraint.GetPtr());
            }
        }
        // The characters take their inner bodies with them.
        characters.reset();
        JPH::BodyInterface& bi = system.GetBodyInterfaceNoLock();
        for (Slot& slot : slots) {
            if (slot.alive && !slot.owned) {
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
    // As find, but not a character's own body: those are the character's to move, reshape and destroy.
    Slot* find_free(Body3D body) {
        Slot* s = find(body);
        return s != nullptr && !s->owned ? s : nullptr;
    }

    ConstraintSlot* find(Constraint3D c) {
        if (c.index >= constraint_slots.size()) {
            return nullptr;
        }
        ConstraintSlot& slot = constraint_slots[c.index];
        return slot.alive && slot.generation == c.generation ? &slot : nullptr;
    }
    const ConstraintSlot* find(Constraint3D c) const { return const_cast<Impl*>(this)->find(c); }

    Body3D handle(u32 slot) const { return {slot, slots[slot].generation}; }
    JPH::Body* body_of(const Slot& slot) { return system.GetBodyLockInterfaceNoLock().TryGetBody(slot.id); }
    const JPH::Body* body_of(const Slot& slot) const { return const_cast<Impl*>(this)->body_of(slot); }
    // The live body a Jolt id stands for, or null.
    const Slot* slot_of(const JPH::BodyID& id) const {
        if (id.GetIndex() >= entries.size()) {
            return nullptr;
        }
        const BodyEntry& e = entries[id.GetIndex()];
        return e.slot != UINT32_MAX && e.id == id && slots[e.slot].alive ? &slots[e.slot] : nullptr;
    }
    u32 slot_index(const JPH::BodyID& id) const {
        const Slot* s = slot_of(id);
        return s != nullptr ? static_cast<u32>(s - slots.data()) : UINT32_MAX;
    }
    JPH::BodyID id_of(Body3D body) const {
        const Slot* s = find(body);
        return s != nullptr ? s->id : JPH::BodyID();
    }

    // Turns the callbacks of the step just done into events, in a fixed order, and updates which pairs touch.
    void collect_events();
    // Disables constraints whose force went over their limit.
    void check_breaks(f32 step_dt);
    void retire_body(Body3D body, bool jolt);
    void push_event(const ContactEvent3D& e) {
        if (events.size() >= config.max_events) {
            ++dropped;
            return;
        }
        events.push_back(e);
    }
    // The force and torque a constraint applied in the last collision step.
    void load_of(const ConstraintSlot& c, f32 dt, f32& force, f32& torque) const;

    JPH::BodyInterface& bi() { return system.GetBodyInterfaceNoLock(); }
    const JPH::BodyInterface& bi() const { return system.GetBodyInterfaceNoLock(); }

    // A Jolt body made elsewhere (a character's inner body) becomes a slot of the world, so that it has a number,
    // takes part in events, queries and the hash like any body.
    Body3D register_body(const JPH::BodyID& id, u64 user_id);

    World3D* self = nullptr;
    std::unique_ptr<physics3d_detail::CharacterSet> characters;

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

    // Jolt's body index to our slot (see BodyEntry).
    std::vector<BodyEntry> entries;
    ContactCollector collector;
    // Touching pairs: key (a << 32 | b) to how many sub shape contacts they have. Ordered, so that anything made from it is.
    std::map<u64, i32> touching;
    // Pairs that went to sleep together: still touching, no longer simulated (see ContactEvent3D). The value counts the
    // steps in which a member was awake and the pair was not seen again.
    std::map<u64, u8> dormant;
    // Pairs a constraint says must not collide, with how many constraints say it.
    std::unordered_map<u64, i32> no_collide;
    std::vector<ContactEvent3D> events;    // since the last drain
    std::vector<ContactEvent3D> queued;    // from destroyed bodies, joined to the next step's batch
    u64 dropped = 0;
    std::vector<ConstraintEvent3D> constraint_events;
    std::vector<ConstraintSlot> constraint_slots;
    std::vector<u32> free_constraint_slots;
    i32 constraints_live = 0;
};

Body3D World3D::Impl::register_body(const JPH::BodyID& id, u64 user_id) {
    u32 index;
    if (!free_slots.empty()) {
        index = free_slots.back();
        free_slots.pop_back();
    } else {
        index = static_cast<u32>(slots.size());
        slots.emplace_back();
    }
    Slot& slot = slots[index];
    slot.id = id;
    if (entries.size() <= id.GetIndex()) {
        entries.resize(id.GetIndex() + 1);
    }
    entries[id.GetIndex()] = {index, id};
    slot.user_id = user_id;
    slot.mass = 0.0f;
    slot.alive = true;
    slot.owned = true;
    slot.material = {};
    slot.is_hold = false;
    slot.hold = {};
    ++live;
    return {index, slot.generation};
}

World3D::World3D(World3DConfig config)
    : _impl(std::make_unique<Impl>(config)) {
    Impl& m = *_impl;
    m.self = this;
    physics3d_detail::CharacterEnv env;
    env.world = this;
    env.system = &m.system;
    env.temp = &m.temp;
    env.table = &m.table;
    Impl* impl = &m;
    env.gravity = [impl] { return from_jolt(impl->system.GetGravity()); };
    env.body_of = [impl](const JPH::BodyID& id) {
        const Impl::Slot* s = impl->slot_of(id);
        return s != nullptr ? Body3D{static_cast<u32>(s - impl->slots.data()), s->generation} : Body3D{};
    };
    env.user_id_of = [impl](Body3D b) {
        const Impl::Slot* s = impl->find(b);
        return s != nullptr ? s->user_id : u64{0};
    };
    env.body_of_slot = [impl](u32 slot) {
        return slot < impl->slots.size() && impl->slots[slot].alive ? impl->handle(slot) : Body3D{};
    };
    env.register_inner = [impl](const JPH::BodyID& id, u64 user_id) { return impl->register_body(id, user_id); };
    env.unregister_inner = [impl](Body3D b) { impl->retire_body(b, false); };
    m.characters = std::make_unique<physics3d_detail::CharacterSet>(std::move(env));
}

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
    Impl& m = *_impl;
    const i32 collision_steps = std::max(1, m.config.collision_steps);
    m.collector.clear();
    // Characters first, in number order (see character3d.hpp): they move through the world as the last step left it,
    // push what they meet by impulses, and the step below then integrates all of it.
    if (m.characters->count() > 0) {
        m.characters->update(dt);
        m.characters->cap_events(m.config.max_events);
    }
    m.system.Update(dt, collision_steps, &m.temp, m.job_system);
    if (m.config.contact_events) {
        m.collect_events();
    }
    if (m.constraints_live > 0) {
        m.check_breaks(dt / static_cast<f32>(collision_steps));
    }
}

void World3D::Impl::collect_events() {
    std::vector<RawContact>& raw = collector.raw();
    std::sort(raw.begin(), raw.end());
    std::vector<ContactEvent3D> batch = std::move(queued);
    queued.clear();
    const auto make = [&](const RawContact& r, ContactKind3D kind) {
        ContactEvent3D e;
        e.a = handle(r.a);
        e.b = handle(r.b);
        e.user_a = slots[r.a].user_id;
        e.user_b = slots[r.b].user_id;
        e.kind = kind;
        e.sensor = r.sensor;
        e.point = r.point;
        e.normal = r.normal;
        e.closing_speed = r.closing;
        e.point_count = r.points;
        return e;
    };
    // Group the sorted records by body pair: (adds, persists, removes) of one pair are adjacent, adds first.
    for (std::size_t i = 0; i < raw.size();) {
        std::size_t j = i;
        i32 adds = 0, removes = 0;
        const RawContact* first_add = nullptr;
        const RawContact* first_persist = nullptr;
        const RawContact* first_remove = nullptr;
        while (j < raw.size() && raw[j].a == raw[i].a && raw[j].b == raw[i].b) {
            const RawContact& r = raw[j];
            if (r.kind == 0) {
                ++adds;
                first_add = first_add != nullptr ? first_add : &r;
            } else if (r.kind == 1) {
                first_persist = first_persist != nullptr ? first_persist : &r;
            } else {
                ++removes;
                first_remove = first_remove != nullptr ? first_remove : &r;
            }
            ++j;
        }
        const u64 key = static_cast<u64>(raw[i].a) << 32 | raw[i].b;
        i32& count = touching[key];
        const i32 before = count;
        count = std::max(0, before + adds - removes);
        const i32 after = count;
        const auto sleeping = [&](u32 slot) { return !bi().IsActive(slots[slot].id); };
        if (before == 0 && after > 0) {
            // A pair that was put to sleep and woke still touching has not begun again.
            if (dormant.erase(key) == 0) {
                batch.push_back(make(*first_add, ContactKind3D::Begin));
            }
        } else if (before > 0 && after == 0 && sleeping(raw[i].a) && sleeping(raw[i].b)) {
            // Both asleep (or static): Jolt forgets the contact, it has not ended.
            dormant[key] = 0;
        } else if (before > 0 && after == 0) {
            // A removal carries no manifold: the pair's last known contact is not kept, so End has no point.
            RawContact r = *first_remove;
            r.point = {};
            r.normal = {};
            r.closing = 0.0f;
            r.points = 0;
            // Whether it was a sensor pair is what the bodies say now (either is a sensor).
            const JPH::Body* ba = body_of(slots[r.a]);
            const JPH::Body* bb = body_of(slots[r.b]);
            r.sensor = (ba != nullptr && ba->IsSensor()) || (bb != nullptr && bb->IsSensor());
            batch.push_back(make(r, ContactKind3D::End));
        } else if (before > 0 && after > 0 && first_persist != nullptr) {
            batch.push_back(make(*first_persist, ContactKind3D::Persist));
        }
        if (after == 0) {
            touching.erase(key);
        }
        i = j;
    }
    // A dormant pair with a member awake that has not been seen touching for two steps has ended.
    for (auto it = dormant.begin(); it != dormant.end();) {
        const u32 a = static_cast<u32>(it->first >> 32), b = static_cast<u32>(it->first & 0xFFFFFFFFu);
        const bool awake = bi().IsActive(slots[a].id) || bi().IsActive(slots[b].id);
        if (awake && ++it->second >= 2) {
            ContactEvent3D e;
            e.a = handle(a);
            e.b = handle(b);
            e.user_a = slots[a].user_id;
            e.user_b = slots[b].user_id;
            e.kind = ContactKind3D::End;
            const JPH::Body* ba = body_of(slots[a]);
            const JPH::Body* bb = body_of(slots[b]);
            e.sensor = (ba != nullptr && ba->IsSensor()) || (bb != nullptr && bb->IsSensor());
            batch.push_back(e);
            it = dormant.erase(it);
        } else {
            ++it;
        }
    }
    std::ranges::sort(batch, [](const ContactEvent3D& x, const ContactEvent3D& y) {
        if (x.a.index != y.a.index) return x.a.index < y.a.index;
        if (x.b.index != y.b.index) return x.b.index < y.b.index;
        if (x.kind != y.kind) return x.kind < y.kind;
        if (x.a.generation != y.a.generation) return x.a.generation < y.a.generation;
        return x.b.generation < y.b.generation;
    });
    for (const ContactEvent3D& e : batch) {
        push_event(e);
    }
    raw.clear();
}

void World3D::Impl::load_of(const ConstraintSlot& c, f32 dt, f32& force, f32& torque) const {
    force = 0.0f;
    torque = 0.0f;
    if (dt <= 0.0f) {
        return;
    }
    const f32 inv = 1.0f / dt;
    const JPH::Constraint* raw = c.constraint.GetPtr();
    switch (c.kind) {
    case Constraint3DKind::Fixed: {
        const auto* k = static_cast<const JPH::FixedConstraint*>(raw);
        force = k->GetTotalLambdaPosition().Length() * inv;
        torque = k->GetTotalLambdaRotation().Length() * inv;
        break;
    }
    case Constraint3DKind::Distance: {
        const auto* k = static_cast<const JPH::DistanceConstraint*>(raw);
        force = std::fabs(k->GetTotalLambdaPosition()) * inv;
        break;
    }
    case Constraint3DKind::Point: {
        const auto* k = static_cast<const JPH::PointConstraint*>(raw);
        force = k->GetTotalLambdaPosition().Length() * inv;
        break;
    }
    case Constraint3DKind::Hinge: {
        const auto* k = static_cast<const JPH::HingeConstraint*>(raw);
        force = k->GetTotalLambdaPosition().Length() * inv;
        torque = k->GetTotalLambdaRotation().Length() * inv;
        break;
    }
    case Constraint3DKind::Cone: {
        const auto* k = static_cast<const JPH::SwingTwistConstraint*>(raw);
        force = k->GetTotalLambdaPosition().Length() * inv;
        const f32 t = k->GetTotalLambdaTwist(), y = k->GetTotalLambdaSwingY(), z = k->GetTotalLambdaSwingZ();
        torque = std::sqrt(t * t + y * y + z * z) * inv;
        break;
    }
    case Constraint3DKind::Slider: {
        const auto* k = static_cast<const JPH::SliderConstraint*>(raw);
        const f32 p = k->GetTotalLambdaPosition().Length(), l = k->GetTotalLambdaPositionLimits();
        force = std::sqrt(p * p + l * l) * inv;
        torque = k->GetTotalLambdaRotation().Length() * inv;
        break;
    }
    }
}

void World3D::Impl::check_breaks(f32 step_dt) {
    // By constraint number: the order of the reports does not depend on anything else.
    for (u32 i = 0; i < constraint_slots.size(); ++i) {
        ConstraintSlot& c = constraint_slots[i];
        if (!c.alive || c.broken || (c.break_force <= 0.0f && c.break_torque <= 0.0f)) {
            continue;
        }
        f32 force = 0.0f, torque = 0.0f;
        load_of(c, step_dt, force, torque);
        if ((c.break_force > 0.0f && force > c.break_force) || (c.break_torque > 0.0f && torque > c.break_torque)) {
            c.broken = true;
            c.constraint->SetEnabled(false);
            ConstraintEvent3D e;
            e.constraint = {i, c.generation};
            e.a = c.a;
            e.b = c.b;
            e.user_id = c.user_id;
            e.force = force;
            e.torque = torque;
            constraint_events.push_back(e);
        }
    }
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
    if (m.entries.size() <= slot.id.GetIndex()) {
        m.entries.resize(slot.id.GetIndex() + 1);
    }
    m.entries[slot.id.GetIndex()] = {index, slot.id};
    slot.user_id = d.user_id;
    slot.mass = mass;
    slot.alive = true;
    slot.material = {};
    slot.is_hold = false;
    slot.hold = {};
    const bool wake = d.start_active && d.motion != Body3DMotion::Static;
    m.bi().AddBody(slot.id, wake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    ++m.live;
    return {index, slot.generation};
}

void World3D::destroy_body(Body3D body) {
    if (_impl->find_free(body) == nullptr) {
        return;
    }
    _impl->retire_body(body, true);
}

// Takes a body out of the world: what it touched wakes, its constraints and pairs end, its slot is freed. `jolt` says
// whether the Jolt body is removed and destroyed here (a character's inner body is the character's own to destroy).
void World3D::Impl::retire_body(Body3D body, bool jolt) {
    Impl& m = *this;
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
    // Constraints on it go with it.
    for (u32 i = 0; i < m.constraint_slots.size(); ++i) {
        const auto& c = m.constraint_slots[i];
        if (c.alive && (c.a == body || c.b == body)) {
            self->destroy_constraint({i, c.generation});
        }
    }
    // Pairs it touches end now (Jolt reports a removal for them too, for an id that no longer names a body: ignored).
    if (m.config.contact_events) {
        for (auto it = m.touching.begin(); it != m.touching.end();) {
            const u32 a = static_cast<u32>(it->first >> 32), b = static_cast<u32>(it->first & 0xFFFFFFFFu);
            if (a == body.index || b == body.index) {
                ContactEvent3D e;
                e.a = m.handle(a);
                e.b = m.handle(b);
                e.user_a = m.slots[a].user_id;
                e.user_b = m.slots[b].user_id;
                e.kind = ContactKind3D::End;
                const JPH::Body* ba = m.body_of(m.slots[a]);
                const JPH::Body* bb = m.body_of(m.slots[b]);
                e.sensor = (ba != nullptr && ba->IsSensor()) || (bb != nullptr && bb->IsSensor());
                m.queued.push_back(e);
                it = m.touching.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = m.dormant.begin(); it != m.dormant.end();) {
            const u32 a = static_cast<u32>(it->first >> 32), b = static_cast<u32>(it->first & 0xFFFFFFFFu);
            if (a == body.index || b == body.index) {
                ContactEvent3D e;
                e.a = m.handle(a);
                e.b = m.handle(b);
                e.user_a = m.slots[a].user_id;
                e.user_b = m.slots[b].user_id;
                e.kind = ContactKind3D::End;
                const JPH::Body* ba = m.body_of(m.slots[a]);
                const JPH::Body* bb = m.body_of(m.slots[b]);
                e.sensor = (ba != nullptr && ba->IsSensor()) || (bb != nullptr && bb->IsSensor());
                m.queued.push_back(e);
                it = m.dormant.erase(it);
            } else {
                ++it;
            }
        }
    }
    m.entries[slot->id.GetIndex()] = {};
    if (jolt) {
        m.bi().RemoveBody(slot->id);
        m.bi().DestroyBody(slot->id);
    }
    slot->alive = false;
    slot->owned = false;
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
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(position)) {
        _impl->bi().SetPosition(s->id, to_jolt(position), JPH::EActivation::Activate);
    }
}

void World3D::set_rotation(Body3D body, Quatf rotation) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(rotation)) {
        _impl->bi().SetRotation(s->id, to_jolt(rotation.normalized()), JPH::EActivation::Activate);
    }
}

void World3D::set_linear_velocity(Body3D body, Vec3f velocity) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(velocity)) {
        _impl->bi().SetLinearVelocity(s->id, to_jolt(velocity));
    }
}

void World3D::set_angular_velocity(Body3D body, Vec3f velocity) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(velocity)) {
        _impl->bi().SetAngularVelocity(s->id, to_jolt(velocity));
    }
}

void World3D::move_kinematic(Body3D body, Vec3f position, Quatf rotation, f32 dt) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(position) && finite(rotation) && dt > 0.0f) {
        _impl->bi().MoveKinematic(s->id, to_jolt(position), to_jolt(rotation.normalized()), dt);
    }
}

void World3D::add_force(Body3D body, Vec3f force) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(force)) {
        _impl->bi().AddForce(s->id, to_jolt(force));
    }
}

void World3D::add_force_at(Body3D body, Vec3f force, Vec3f world_point) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(force) && finite(world_point)) {
        _impl->bi().AddForce(s->id, to_jolt(force), to_jolt(world_point));
    }
}

void World3D::add_torque(Body3D body, Vec3f torque) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(torque)) {
        _impl->bi().AddTorque(s->id, to_jolt(torque));
    }
}

void World3D::add_impulse(Body3D body, Vec3f impulse) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(impulse)) {
        _impl->bi().AddImpulse(s->id, to_jolt(impulse));
    }
}

void World3D::add_impulse_at(Body3D body, Vec3f impulse, Vec3f world_point) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(impulse) && finite(world_point)) {
        _impl->bi().AddImpulse(s->id, to_jolt(impulse), to_jolt(world_point));
    }
}

void World3D::add_angular_impulse(Body3D body, Vec3f impulse) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(impulse)) {
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
    if (auto* s = _impl->find_free(body)) {
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
    if (const auto* s = _impl->find_free(body)) {
        _impl->bi().ActivateBody(s->id);
    }
}

void World3D::deactivate(Body3D body) {
    if (const auto* s = _impl->find_free(body)) {
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

// --- helpers for driven bodies ---

void World3D::teleport(Body3D body, Vec3f position, Quatf rotation) {
    if (const auto* s = _impl->find_free(body); s != nullptr && finite(position) && finite(rotation)) {
        _impl->bi().SetPositionAndRotation(s->id, to_jolt(position), to_jolt(rotation.normalized()),
                                           JPH::EActivation::Activate);
        _impl->bi().SetLinearAndAngularVelocity(s->id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    }
}

void World3D::follow_transform(Body3D body, Vec3f position, Quatf rotation, f32 dt) {
    Impl& m = *_impl;
    const auto* s = m.find_free(body);
    if (s == nullptr || !finite(position) || !finite(rotation) || !(dt > 0.0f)) {
        return;
    }
    const Quatf target = rotation.normalized();
    switch (m.bi().GetMotionType(s->id)) {
    case JPH::EMotionType::Kinematic:
        m.bi().MoveKinematic(s->id, to_jolt(position), to_jolt(target), dt);
        return;
    case JPH::EMotionType::Static:
        m.bi().SetPositionAndRotation(s->id, to_jolt(position), to_jolt(target), JPH::EActivation::DontActivate);
        return;
    case JPH::EMotionType::Dynamic:
        break;
    }
    // Velocities that reach the target in dt: the centre of mass goes where it will be, the rotation by the short way.
    const JPH::Vec3 com_local = m.bi().GetShape(s->id)->GetCenterOfMass();
    const JPH::Vec3 now = m.bi().GetCenterOfMassPosition(s->id);
    const JPH::Vec3 then = to_jolt(position) + to_jolt(target) * com_local;
    const JPH::Quat current = m.bi().GetRotation(s->id);
    JPH::Quat delta = to_jolt(target) * current.Conjugated();
    if (delta.GetW() < 0.0f) {
        delta = -delta;
    }
    JPH::Vec3 axis;
    f32 angle;
    delta.GetAxisAngle(axis, angle);
    m.bi().SetLinearAndAngularVelocity(s->id, (then - now) / dt, axis * (angle / dt));
    m.bi().ActivateBody(s->id);
}

bool World3D::set_layer(Body3D body, u8 layer) {
    Impl& m = *_impl;
    const auto* s = m.find_free(body);
    if (s == nullptr) {
        return false;
    }
    const bool is_static = m.bi().GetMotionType(s->id) == JPH::EMotionType::Static;
    if (layer == layers3d::Auto) {
        layer = is_static ? layers3d::Static
              : m.bi().GetMotionType(s->id) == JPH::EMotionType::Kinematic ? layers3d::Kinematic : layers3d::Dynamic;
    }
    if (layer >= layers3d::Count || (layer == layers3d::Static && !is_static)) {
        return false;
    }
    m.bi().SetObjectLayer(s->id, JPH::ObjectLayer(layer));
    return true;
}

bool World3D::is_sensor(Body3D body) const {
    const auto* s = _impl->find_free(body);
    return s != nullptr && _impl->bi().IsSensor(s->id);
}

void World3D::set_sensor(Body3D body, bool sensor) {
    if (const auto* s = _impl->find_free(body)) {
        _impl->bi().SetIsSensor(s->id, sensor);
        _impl->bi().ActivateBody(s->id);
    }
}

bool World3D::set_shape(Body3D body, const Shape3D& shape) {
    Impl& m = *_impl;
    auto* s = m.find_free(body);
    if (s == nullptr || !shape.valid()) {
        return false;
    }
    const Shape3DKind kind = shape.kind();
    const bool surface = kind == Shape3DKind::TriangleMesh || kind == Shape3DKind::HeightField;
    const bool dynamic = m.bi().GetMotionType(s->id) == JPH::EMotionType::Dynamic;
    if (dynamic && surface) {
        return false;
    }
    m.bi().SetShape(s->id, shape._data->shape.GetPtr(), false, JPH::EActivation::Activate);
    if (dynamic) {
        // The mass stays what it was made with; the inertia is the new shape's at that mass.
        if (JPH::Body* b = m.body_of(*s)) {
            JPH::MassProperties mp = shape._data->shape->GetMassProperties();
            mp.ScaleToMass(s->mass);
            b->GetMotionProperties()->SetMassProperties(JPH::EAllowedDOFs::All, mp);
        }
    }
    return true;
}

std::vector<Body3D> World3D::bodies() const {
    std::vector<Body3D> out;
    out.reserve(static_cast<std::size_t>(_impl->live));
    for (u32 i = 0; i < _impl->slots.size(); ++i) {
        if (_impl->slots[i].alive) {
            out.push_back(_impl->handle(i));
        }
    }
    return out;
}

// --- queries ---

namespace {

// The child a sub shape id is inside, in the order the children were given (0 when the shape is not a compound).
u32 child_index(const JPH::Shape* shape, JPH::SubShapeID id) {
    if (shape != nullptr && shape->GetType() == JPH::EShapeType::Compound) {
        const auto* compound = static_cast<const JPH::CompoundShape*>(shape);
        JPH::SubShapeID rest;
        const JPH::uint index = compound->GetSubShapeIndexFromID(id, rest);
        if (index < compound->GetNumSubShapes()) {
            return compound->GetCompoundUserData(index);
        }
    }
    return 0;
}

} // namespace

std::optional<RayHit3D> World3D::raycast(Vec3f from, Vec3f to, const QueryFilter3D& filter) const {
    const Impl& m = *_impl;
    const Vec3f d = to - from;
    if (!finite(from) || !finite(to) || !(length(d) > 0.0f)) {
        return std::nullopt;
    }
    const JPH::RRayCast ray(to_jolt(from), to_jolt(d));
    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces; // meshes are double sided
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    m.system.GetNarrowPhaseQueryNoLock().CastRay(ray, settings, collector, {}, MaskLayerFilter(filter.mask),
                                                 QueryBodyFilter(m.id_of(filter.ignore), filter.sensors));
    if (!collector.HadHit()) {
        return std::nullopt;
    }
    const JPH::RayCastResult& hit = collector.mHit;
    const u32 slot = m.slot_index(hit.mBodyID);
    if (slot == UINT32_MAX) {
        return std::nullopt;
    }
    RayHit3D out;
    out.body = m.handle(slot);
    out.user_id = m.slots[slot].user_id;
    const JPH::Vec3 point = ray.GetPointOnRay(hit.mFraction);
    const JPH::TransformedShape ts = m.bi().GetTransformedShape(hit.mBodyID);
    out.point = from_jolt(point);
    out.normal = from_jolt(ts.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
    out.fraction = hit.mFraction;
    out.distance = hit.mFraction * length(d);
    out.sub_shape = child_index(ts.mShape.GetPtr(), hit.mSubShapeID2);
    return out;
}

std::vector<RayHit3D> World3D::raycast_all(Vec3f from, Vec3f to, const QueryFilter3D& filter) const {
    const Impl& m = *_impl;
    std::vector<RayHit3D> out;
    const Vec3f d = to - from;
    if (!finite(from) || !finite(to) || !(length(d) > 0.0f)) {
        return out;
    }
    const JPH::RRayCast ray(to_jolt(from), to_jolt(d));
    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    m.system.GetNarrowPhaseQueryNoLock().CastRay(ray, settings, collector, {}, MaskLayerFilter(filter.mask),
                                                 QueryBodyFilter(m.id_of(filter.ignore), filter.sensors));
    struct Keyed {
        RayHit3D hit;
        u32 raw_sub;
    };
    std::vector<Keyed> keyed;
    keyed.reserve(collector.mHits.size());
    const f32 len = length(d);
    for (const JPH::RayCastResult& hit : collector.mHits) {
        const u32 slot = m.slot_index(hit.mBodyID);
        if (slot == UINT32_MAX) {
            continue;
        }
        Keyed k;
        k.hit.body = m.handle(slot);
        k.hit.user_id = m.slots[slot].user_id;
        const JPH::Vec3 point = ray.GetPointOnRay(hit.mFraction);
        const JPH::TransformedShape ts = m.bi().GetTransformedShape(hit.mBodyID);
        k.hit.point = from_jolt(point);
        k.hit.normal = from_jolt(ts.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
        k.hit.fraction = hit.mFraction;
        k.hit.distance = hit.mFraction * len;
        k.hit.sub_shape = child_index(ts.mShape.GetPtr(), hit.mSubShapeID2);
        k.raw_sub = hit.mSubShapeID2.GetValue();
        keyed.push_back(k);
    }
    std::ranges::sort(keyed, [](const Keyed& x, const Keyed& y) {
        if (x.hit.fraction != y.hit.fraction) return x.hit.fraction < y.hit.fraction;
        if (x.hit.body.index != y.hit.body.index) return x.hit.body.index < y.hit.body.index;
        return x.raw_sub < y.raw_sub;
    });
    out.reserve(keyed.size());
    for (const Keyed& k : keyed) {
        out.push_back(k.hit);
    }
    return out;
}

std::optional<ShapeHit3D> World3D::shape_cast(const Shape3D& shape, Vec3f position, Quatf rotation, Vec3f displacement,
                                              const QueryFilter3D& filter) const {
    const Impl& m = *_impl;
    if (!shape.valid() || !finite(position) || !finite(rotation) || !finite(displacement)) {
        return std::nullopt;
    }
    const Shape3DKind kind = shape.kind();
    if (kind == Shape3DKind::TriangleMesh || kind == Shape3DKind::HeightField) {
        return std::nullopt;
    }
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        shape._data->shape.GetPtr(), JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sRotationTranslation(to_jolt(rotation.normalized()), to_jolt(position)), to_jolt(displacement));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    m.system.GetNarrowPhaseQueryNoLock().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {},
                                                   MaskLayerFilter(filter.mask),
                                                   QueryBodyFilter(m.id_of(filter.ignore), filter.sensors));
    if (!collector.HadHit()) {
        return std::nullopt;
    }
    const JPH::ShapeCastResult& hit = collector.mHit;
    const u32 slot = m.slot_index(hit.mBodyID2);
    if (slot == UINT32_MAX) {
        return std::nullopt;
    }
    ShapeHit3D out;
    out.body = m.handle(slot);
    out.user_id = m.slots[slot].user_id;
    out.fraction = hit.mFraction;
    out.position = position + displacement * hit.mFraction;
    out.point = from_jolt(hit.mContactPointOn2);
    out.normal = normalize(-from_jolt(hit.mPenetrationAxis));
    out.penetration = hit.mFraction <= 0.0f ? std::max(0.0f, hit.mPenetrationDepth) : 0.0f;
    out.sub_shape = child_index(m.bi().GetShape(hit.mBodyID2).GetPtr(), hit.mSubShapeID2);
    return out;
}

std::vector<OverlapHit3D> World3D::overlap(const Shape3D& shape, Vec3f position, Quatf rotation,
                                           const QueryFilter3D& filter) const {
    const Impl& m = *_impl;
    std::vector<OverlapHit3D> out;
    if (!shape.valid() || !finite(position) || !finite(rotation)) {
        return out;
    }
    const JPH::Shape* jolt = shape._data->shape.GetPtr();
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(to_jolt(rotation.normalized()), to_jolt(position))
                                      .PreTranslated(jolt->GetCenterOfMass());
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    m.system.GetNarrowPhaseQueryNoLock().CollideShape(jolt, JPH::Vec3::sReplicate(1.0f), transform, settings,
                                                      JPH::RVec3::sZero(), collector, {}, MaskLayerFilter(filter.mask),
                                                      QueryBodyFilter(m.id_of(filter.ignore), filter.sensors));
    // One entry for each body: the deepest of its contacts (ties: the lower sub shape id).
    struct Deepest {
        OverlapHit3D hit;
        u32 raw_sub = 0;
    };
    std::map<u32, Deepest> by_body;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const u32 slot = m.slot_index(r.mBodyID2);
        if (slot == UINT32_MAX) {
            continue;
        }
        const u32 raw_sub = r.mSubShapeID2.GetValue();
        auto [it, inserted] = by_body.try_emplace(slot);
        Deepest& d = it->second;
        if (inserted || r.mPenetrationDepth > d.hit.penetration
            || (r.mPenetrationDepth == d.hit.penetration && raw_sub < d.raw_sub)) {
            d.hit.body = m.handle(slot);
            d.hit.user_id = m.slots[slot].user_id;
            d.hit.penetration = r.mPenetrationDepth;
            d.hit.point = from_jolt(r.mContactPointOn2);
            d.hit.normal = normalize(-from_jolt(r.mPenetrationAxis));
            d.raw_sub = raw_sub;
        }
    }
    for (auto& [slot, d] : by_body) {
        out.push_back(d.hit);
    }
    return out;
}

std::vector<Body3D> World3D::point_inside(Vec3f point, const QueryFilter3D& filter) const {
    const Impl& m = *_impl;
    std::vector<Body3D> out;
    if (!finite(point)) {
        return out;
    }
    JPH::AllHitCollisionCollector<JPH::CollidePointCollector> collector;
    m.system.GetNarrowPhaseQueryNoLock().CollidePoint(to_jolt(point), collector, {}, MaskLayerFilter(filter.mask),
                                                      QueryBodyFilter(m.id_of(filter.ignore), filter.sensors));
    for (const JPH::CollidePointResult& r : collector.mHits) {
        if (const u32 slot = m.slot_index(r.mBodyID); slot != UINT32_MAX) {
            out.push_back(m.handle(slot));
        }
    }
    std::ranges::sort(out, [](Body3D x, Body3D y) { return x.index < y.index; });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<ClosestPoint3D> World3D::closest_point(Vec3f point, f32 max_distance, const QueryFilter3D& filter) const {
    const Impl& m = *_impl;
    if (!finite(point) || !(max_distance >= 0.0f) || !std::isfinite(max_distance)) {
        return std::nullopt;
    }
    // A sphere of a millimetre: its contact with a surface is the nearest point of it.
    constexpr f32 probe = 0.001f;
    JPH::SphereShape sphere(probe);
    sphere.SetEmbedded();
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = max_distance;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    m.system.GetNarrowPhaseQueryNoLock().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f),
                                                      JPH::RMat44::sTranslation(to_jolt(point)), settings,
                                                      JPH::RVec3::sZero(), collector, {}, MaskLayerFilter(filter.mask),
                                                      QueryBodyFilter(m.id_of(filter.ignore), filter.sensors));
    std::optional<ClosestPoint3D> best;
    u32 best_sub = 0;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const u32 slot = m.slot_index(r.mBodyID2);
        if (slot == UINT32_MAX) {
            continue;
        }
        ClosestPoint3D c;
        c.body = m.handle(slot);
        c.user_id = m.slots[slot].user_id;
        if (r.mPenetrationDepth > probe) { // the centre is inside the solid
            c.point = point;
            c.distance = 0.0f;
        } else {
            c.point = from_jolt(r.mContactPointOn2);
            c.distance = std::max(0.0f, length(c.point - point));
        }
        const u32 raw_sub = r.mSubShapeID2.GetValue();
        if (!best || c.distance < best->distance
            || (c.distance == best->distance
                && (c.body.index < best->body.index || (c.body.index == best->body.index && raw_sub < best_sub)))) {
            best = c;
            best_sub = raw_sub;
        }
    }
    return best;
}

// --- events ---

std::span<const ContactEvent3D> World3D::events() const { return _impl->events; }

std::vector<ContactEvent3D> World3D::drain_events() {
    std::vector<ContactEvent3D> out = std::move(_impl->events);
    _impl->events.clear();
    return out;
}

u64 World3D::dropped_events() const { return _impl->dropped; }

std::vector<ConstraintEvent3D> World3D::drain_constraint_events() {
    std::vector<ConstraintEvent3D> out = std::move(_impl->constraint_events);
    _impl->constraint_events.clear();
    return out;
}

// --- constraints ---

Constraint3D World3D::create_constraint(const Constraint3DDesc& d) {
    Impl& m = *_impl;
    const auto* sb = m.find_free(d.b);
    const auto* sa = d.a.valid() ? m.find_free(d.a) : nullptr;
    if (sb == nullptr || (d.a.valid() && sa == nullptr) || d.a == d.b) {
        return {};
    }
    if (!finite(d.point) || !finite(d.point_b) || !finite(d.axis) || !std::isfinite(d.min_distance)
        || !std::isfinite(d.max_distance) || !std::isfinite(d.spring_frequency) || !std::isfinite(d.spring_damping)
        || !std::isfinite(d.min_angle) || !std::isfinite(d.max_angle) || !std::isfinite(d.swing_angle)
        || !std::isfinite(d.max_friction) || !std::isfinite(d.break_force) || !std::isfinite(d.break_torque)) {
        return {};
    }
    const bool needs_axis = d.kind == Constraint3DKind::Hinge || d.kind == Constraint3DKind::Cone
                            || d.kind == Constraint3DKind::Slider;
    JPH::Vec3 axis = to_jolt(d.axis);
    if (needs_axis) {
        if (axis.LengthSq() < 1e-12f) {
            return {};
        }
        axis = axis.Normalized();
    }
    const JPH::Vec3 normal = axis.GetNormalizedPerpendicular();
    const JPH::RVec3 p = to_jolt(d.point);

    JPH::Body* body_a = sa != nullptr ? m.body_of(*sa) : &JPH::Body::sFixedToWorld;
    JPH::Body* body_b = m.body_of(*sb);
    if (body_a == nullptr || body_b == nullptr) {
        return {};
    }
    JPH::Ref<JPH::TwoBodyConstraint> made;
    switch (d.kind) {
    case Constraint3DKind::Fixed: {
        JPH::FixedConstraintSettings s;
        s.mPoint1 = s.mPoint2 = p;
        made = s.Create(*body_a, *body_b);
        break;
    }
    case Constraint3DKind::Point: {
        JPH::PointConstraintSettings s;
        s.mPoint1 = s.mPoint2 = p;
        made = s.Create(*body_a, *body_b);
        break;
    }
    case Constraint3DKind::Distance: {
        JPH::DistanceConstraintSettings s;
        s.mPoint1 = p;
        s.mPoint2 = to_jolt(d.point_b);
        s.mMinDistance = d.min_distance;
        s.mMaxDistance = d.max_distance;
        if (d.spring_frequency > 0.0f) {
            s.mLimitsSpringSettings.mMode = JPH::ESpringMode::FrequencyAndDamping;
            s.mLimitsSpringSettings.mFrequency = d.spring_frequency;
            s.mLimitsSpringSettings.mDamping = d.spring_damping;
        }
        made = s.Create(*body_a, *body_b);
        break;
    }
    case Constraint3DKind::Hinge: {
        JPH::HingeConstraintSettings s;
        s.mPoint1 = s.mPoint2 = p;
        s.mHingeAxis1 = s.mHingeAxis2 = axis;
        s.mNormalAxis1 = s.mNormalAxis2 = normal;
        if (d.limits) {
            s.mLimitsMin = to_radians(std::clamp(d.min_angle, -180.0f, 0.0f));
            s.mLimitsMax = to_radians(std::clamp(d.max_angle, 0.0f, 180.0f));
            if (d.spring_frequency > 0.0f) {
                s.mLimitsSpringSettings.mFrequency = d.spring_frequency;
                s.mLimitsSpringSettings.mDamping = d.spring_damping;
            }
        }
        s.mMaxFrictionTorque = std::max(0.0f, d.max_friction);
        made = s.Create(*body_a, *body_b);
        break;
    }
    case Constraint3DKind::Cone: {
        JPH::SwingTwistConstraintSettings s;
        s.mPosition1 = s.mPosition2 = p;
        s.mTwistAxis1 = s.mTwistAxis2 = axis;
        s.mPlaneAxis1 = s.mPlaneAxis2 = normal;
        s.mNormalHalfConeAngle = s.mPlaneHalfConeAngle = to_radians(std::clamp(d.swing_angle, 0.0f, 180.0f));
        if (d.limits) {
            s.mTwistMinAngle = to_radians(std::clamp(d.min_angle, -180.0f, 0.0f));
            s.mTwistMaxAngle = to_radians(std::clamp(d.max_angle, 0.0f, 180.0f));
        } else {
            s.mTwistMinAngle = -pi_f;
            s.mTwistMaxAngle = pi_f;
        }
        s.mMaxFrictionTorque = std::max(0.0f, d.max_friction);
        made = s.Create(*body_a, *body_b);
        break;
    }
    case Constraint3DKind::Slider: {
        JPH::SliderConstraintSettings s;
        s.mPoint1 = s.mPoint2 = p;
        s.SetSliderAxis(axis);
        if (d.limits) {
            s.mLimitsMin = std::min(0.0f, d.min_position);
            s.mLimitsMax = std::max(0.0f, d.max_position);
            if (d.spring_frequency > 0.0f) {
                s.mLimitsSpringSettings.mFrequency = d.spring_frequency;
                s.mLimitsSpringSettings.mDamping = d.spring_damping;
            }
        }
        s.mMaxFrictionForce = std::max(0.0f, d.max_friction);
        made = s.Create(*body_a, *body_b);
        break;
    }
    }
    if (made == nullptr) {
        return {};
    }
    u32 index;
    if (!m.free_constraint_slots.empty()) {
        index = m.free_constraint_slots.back();
        m.free_constraint_slots.pop_back();
    } else {
        index = static_cast<u32>(m.constraint_slots.size());
        m.constraint_slots.emplace_back();
    }
    Impl::ConstraintSlot& c = m.constraint_slots[index];
    c.constraint = made;
    c.kind = d.kind;
    c.a = d.a;
    c.b = d.b;
    // Anchors in each body's own frame (its origin and rotation).
    const auto local = [&](const JPH::Body* body, Vec3f world_point) {
        if (body == &JPH::Body::sFixedToWorld) {
            return world_point;
        }
        return from_jolt(body->GetRotation().Conjugated() * (to_jolt(world_point) - JPH::Vec3(body->GetPosition())));
    };
    c.local_a = local(body_a, d.point);
    c.local_b = local(body_b, d.kind == Constraint3DKind::Distance ? d.point_b : d.point);
    c.local_axis = needs_axis ? (body_a == &JPH::Body::sFixedToWorld
                                    ? from_jolt(axis)
                                    : from_jolt(body_a->GetRotation().Conjugated() * axis))
                              : Vec3f{};
    c.break_force = std::max(0.0f, d.break_force);
    c.break_torque = std::max(0.0f, d.break_torque);
    c.user_id = d.user_id;
    c.alive = true;
    c.broken = false;
    c.no_collide = !d.collide_connected && d.a.valid();
    if (c.no_collide) {
        ++m.no_collide[static_cast<u64>(std::min(d.a.index, d.b.index)) << 32 | std::max(d.a.index, d.b.index)];
    }
    m.system.AddConstraint(made.GetPtr());
    ++m.constraints_live;
    // A constraint is for bodies that move: wake them.
    if (sa != nullptr) {
        m.bi().ActivateBody(sa->id);
    }
    m.bi().ActivateBody(sb->id);
    return {index, c.generation};
}

void World3D::destroy_constraint(Constraint3D constraint) {
    Impl& m = *_impl;
    Impl::ConstraintSlot* c = m.find(constraint);
    if (c == nullptr) {
        return;
    }
    if (c->no_collide) {
        const u64 key = static_cast<u64>(std::min(c->a.index, c->b.index)) << 32 | std::max(c->a.index, c->b.index);
        if (--m.no_collide[key] <= 0) {
            m.no_collide.erase(key);
        }
        c->no_collide = false;
    }
    m.system.RemoveConstraint(c->constraint.GetPtr());
    c->constraint = nullptr;
    c->alive = false;
    ++c->generation;
    m.free_constraint_slots.push_back(constraint.index);
    --m.constraints_live;
}

bool World3D::contains(Constraint3D constraint) const { return _impl->find(constraint) != nullptr; }
i32 World3D::constraint_count() const { return _impl->constraints_live; }

Constraint3DInfo World3D::constraint_info(Constraint3D constraint) const {
    const Impl& m = *_impl;
    Constraint3DInfo out;
    const Impl::ConstraintSlot* c = m.find(constraint);
    if (c == nullptr) {
        return out;
    }
    const auto world_point = [&](Body3D body, Vec3f local) {
        const auto* s = m.find(body);
        return s != nullptr ? from_jolt(JPH::Vec3(m.bi().GetPosition(s->id)) + m.bi().GetRotation(s->id) * to_jolt(local))
                            : local;
    };
    out.kind = c->kind;
    out.a = c->a;
    out.b = c->b;
    out.point_a = world_point(c->a, c->local_a);
    out.point_b = world_point(c->b, c->local_b);
    if (c->kind == Constraint3DKind::Hinge || c->kind == Constraint3DKind::Cone || c->kind == Constraint3DKind::Slider) {
        const auto* sa = c->a.valid() ? m.find(c->a) : nullptr;
        out.axis = sa != nullptr ? from_jolt(m.bi().GetRotation(sa->id) * to_jolt(c->local_axis)) : c->local_axis;
    }
    out.broken = c->broken;
    out.enabled = c->constraint->GetEnabled();
    out.user_id = c->user_id;
    switch (c->kind) {
    case Constraint3DKind::Hinge:
        out.value = to_degrees(static_cast<const JPH::HingeConstraint*>(c->constraint.GetPtr())->GetCurrentAngle());
        break;
    case Constraint3DKind::Slider:
        out.value = static_cast<const JPH::SliderConstraint*>(c->constraint.GetPtr())->GetCurrentPosition();
        break;
    case Constraint3DKind::Distance:
        out.value = length(out.point_b - out.point_a);
        break;
    default: break;
    }
    return out;
}

void World3D::set_constraint_motor(Constraint3D constraint, Motor3DMode mode, f32 target, f32 max_force) {
    Impl::ConstraintSlot* c = _impl->find(constraint);
    if (c == nullptr || !std::isfinite(target) || !std::isfinite(max_force)) {
        return;
    }
    const JPH::EMotorState state = mode == Motor3DMode::Off ? JPH::EMotorState::Off
                                 : mode == Motor3DMode::Velocity ? JPH::EMotorState::Velocity : JPH::EMotorState::Position;
    if (mode != Motor3DMode::Off && !(max_force > 0.0f)) {
        return;
    }
    if (c->kind == Constraint3DKind::Hinge) {
        auto* h = static_cast<JPH::HingeConstraint*>(c->constraint.GetPtr());
        if (mode != Motor3DMode::Off) {
            h->GetMotorSettings().SetTorqueLimit(max_force);
            h->GetMotorSettings().mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, 10.0f, 1.0f);
            if (mode == Motor3DMode::Velocity) {
                h->SetTargetAngularVelocity(to_radians(target));
            } else {
                h->SetTargetAngle(to_radians(target));
            }
        }
        h->SetMotorState(state);
    } else if (c->kind == Constraint3DKind::Slider) {
        auto* sl = static_cast<JPH::SliderConstraint*>(c->constraint.GetPtr());
        if (mode != Motor3DMode::Off) {
            sl->GetMotorSettings().SetForceLimit(max_force);
            sl->GetMotorSettings().mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, 10.0f, 1.0f);
            if (mode == Motor3DMode::Velocity) {
                sl->SetTargetVelocity(target);
            } else {
                sl->SetTargetPosition(target);
            }
        }
        sl->SetMotorState(state);
    } else {
        return;
    }
    if (const auto* sb = _impl->find(c->b)) {
        _impl->bi().ActivateBody(sb->id);
    }
    if (const auto* sa = c->a.valid() ? _impl->find(c->a) : nullptr) {
        _impl->bi().ActivateBody(sa->id);
    }
}

void World3D::set_constraint_enabled(Constraint3D constraint, bool enabled) {
    Impl::ConstraintSlot* c = _impl->find(constraint);
    if (c == nullptr) {
        return;
    }
    c->constraint->SetEnabled(enabled);
    if (enabled) {
        c->broken = false;
    }
}

std::vector<Constraint3D> World3D::constraints() const {
    std::vector<Constraint3D> out;
    for (u32 i = 0; i < _impl->constraint_slots.size(); ++i) {
        if (_impl->constraint_slots[i].alive) {
            out.push_back({i, _impl->constraint_slots[i].generation});
        }
    }
    return out;
}

// --- characters ---

Character3D World3D::create_character(const CharacterDesc3D& desc) {
    if (_impl->live >= static_cast<i32>(_impl->config.max_bodies)) {
        return {};
    }
    return _impl->characters->create(desc);
}
void World3D::destroy_character(Character3D c) { _impl->characters->destroy(c); }
bool World3D::contains(Character3D c) const { return _impl->characters->contains(c); }
i32 World3D::character_count() const { return _impl->characters->count(); }
std::vector<Character3D> World3D::characters() const { return _impl->characters->list(); }
void World3D::set_character_input(Character3D c, const CharacterInput3D& input) { _impl->characters->set_input(c, input); }
CharacterInput3D World3D::character_input(Character3D c) const { return _impl->characters->input(c); }
CharacterState3D World3D::character(Character3D c) const { return _impl->characters->state(c); }
std::vector<CharacterContact3D> World3D::character_contacts(Character3D c) const { return _impl->characters->contacts(c); }
void World3D::teleport_character(Character3D c, Vec3f feet) { _impl->characters->teleport(c, feet); }
void World3D::set_character_up(Character3D c, Vec3f up, bool instant) { _impl->characters->set_up(c, up, instant); }
bool World3D::set_character_crouch(Character3D c, bool crouch) { return _impl->characters->set_crouch(c, crouch); }
void World3D::set_character_desc(Character3D c, const CharacterDesc3D& desc) { _impl->characters->set_desc(c, desc); }
void World3D::set_traversal_input(Character3D c, const TraversalInput3D& input) { _impl->characters->set_traversal_input(c, input); }
TraversalInput3D World3D::traversal_input(Character3D c) const { return _impl->characters->traversal_input(c); }
TraversalState3D World3D::traversal(Character3D c) const { return _impl->characters->traversal(c); }
void World3D::set_traversal_resource(Character3D c, f32 value) { _impl->characters->set_traversal_resource(c, value); }

// --- grab and climb: what a body is to a hand ---

void World3D::set_surface_material(Body3D body, const SurfaceMaterial3D& material) {
    Impl::Slot* s = _impl->find(body);
    if (s == nullptr || !std::isfinite(material.grip) || !std::isfinite(material.slip) || !std::isfinite(material.drain)) {
        return;
    }
    s->material = material;
}

SurfaceMaterial3D World3D::surface_material(Body3D body) const {
    const Impl::Slot* s = _impl->find(body);
    return s != nullptr ? s->material : SurfaceMaterial3D{};
}

bool World3D::set_hold(Body3D body, const HoldDesc3D& hold) {
    Impl::Slot* s = _impl->find(body);
    if (s == nullptr || !std::isfinite(hold.quality) || !std::isfinite(hold.drain) || !finite(hold.anchor)
        || !finite(hold.anchor_normal)) {
        return false;
    }
    s->is_hold = true;
    s->hold = hold;
    s->hold.anchor_normal = normalize(hold.anchor_normal) == Vec3f{} ? Vec3f{0.0f, 0.0f, 1.0f} : normalize(hold.anchor_normal);
    return true;
}

void World3D::clear_hold(Body3D body) {
    if (Impl::Slot* s = _impl->find(body)) {
        s->is_hold = false;
        s->hold = {};
    }
}

bool World3D::is_hold(Body3D body) const {
    const Impl::Slot* s = _impl->find(body);
    return s != nullptr && s->is_hold;
}

HoldDesc3D World3D::hold(Body3D body) const {
    const Impl::Slot* s = _impl->find(body);
    return s != nullptr && s->is_hold ? s->hold : HoldDesc3D{};
}

std::span<const CharacterEvent3D> World3D::character_events() const { return _impl->characters->events(); }
std::vector<CharacterEvent3D> World3D::drain_character_events() { return _impl->characters->drain_events(); }

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
    // Constraints, only when there are some (a world without keeps the hash it had before they existed).
    if (m.constraints_live > 0) {
        h.u32v(static_cast<u32>(m.constraints_live));
        for (u32 i = 0; i < m.constraint_slots.size(); ++i) {
            const Impl::ConstraintSlot& c = m.constraint_slots[i];
            if (!c.alive) {
                continue;
            }
            h.u32v(i);
            h.u32v(c.generation);
            h.byte(static_cast<u8>(c.kind));
            h.u32v(c.a.index);
            h.u32v(c.b.index);
            h.u64v(c.user_id);
            h.byte(c.broken ? 1 : 0);
            h.byte(c.constraint->GetEnabled() ? 1 : 0);
        }
    }
    // Characters, only when there are some: their own state (the input, the up vector, timers, what they stand on),
    // which the inner bodies' places above do not carry.
    if (m.characters->count() > 0) {
        std::vector<u8> bytes;
        m.characters->save_header(bytes);
        for (const u8 b : bytes) {
            h.byte(b);
        }
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
    // Constraints: which exist and whether they broke (Jolt keeps their motors and whether they are enabled), and
    // which pairs touch, so that a restored world does not report a Begin again for a pair that was already in contact.
    const u32 cs = static_cast<u32>(m.constraint_slots.size());
    out.WriteBytes(&cs, sizeof cs);
    for (const Impl::ConstraintSlot& c : m.constraint_slots) {
        const u32 generation = c.generation;
        const u32 flags = (c.alive ? 1u : 0u) | (c.broken ? 2u : 0u);
        out.WriteBytes(&generation, sizeof generation);
        out.WriteBytes(&flags, sizeof flags);
    }
    const u32 pairs = static_cast<u32>(m.touching.size());
    out.WriteBytes(&pairs, sizeof pairs);
    for (const auto& [key, count] : m.touching) {
        out.WriteBytes(&key, sizeof key);
        out.WriteBytes(&count, sizeof count);
    }
    const u32 sleepers = static_cast<u32>(m.dormant.size());
    out.WriteBytes(&sleepers, sizeof sleepers);
    for (const auto& [key, waited] : m.dormant) {
        out.WriteBytes(&key, sizeof key);
        out.WriteBytes(&waited, sizeof waited);
    }
    // The characters' own state, then Jolt's after the physics system's.
    m.characters->save_header(out.bytes());
    m.system.SaveState(out);
    m.characters->save_jolt(out);
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
    u32 cs = 0;
    in.ReadBytes(&cs, sizeof cs);
    if (in.IsFailed() || cs != m.constraint_slots.size()) {
        return false;
    }
    std::vector<u32> constraint_flags(cs);
    for (u32 i = 0; i < cs; ++i) {
        u32 generation = 0;
        in.ReadBytes(&generation, sizeof generation);
        in.ReadBytes(&constraint_flags[i], sizeof(u32));
        if (in.IsFailed() || generation != m.constraint_slots[i].generation
            || ((constraint_flags[i] & 1u) != 0) != m.constraint_slots[i].alive) {
            return false;
        }
    }
    u32 pairs = 0;
    in.ReadBytes(&pairs, sizeof pairs);
    if (in.IsFailed() || pairs > (1u << 24)) {
        return false;
    }
    std::map<u64, i32> touching;
    for (u32 i = 0; i < pairs; ++i) {
        u64 key = 0;
        i32 n = 0;
        in.ReadBytes(&key, sizeof key);
        in.ReadBytes(&n, sizeof n);
        touching[key] = n;
    }
    u32 sleepers = 0;
    in.ReadBytes(&sleepers, sizeof sleepers);
    if (in.IsFailed() || sleepers > (1u << 24)) {
        return false;
    }
    std::map<u64, u8> dormant;
    for (u32 i = 0; i < sleepers; ++i) {
        u64 key = 0;
        u8 waited = 0;
        in.ReadBytes(&key, sizeof key);
        in.ReadBytes(&waited, sizeof waited);
        dormant[key] = waited;
    }
    if (in.IsFailed()) {
        return false;
    }
    // The characters' own state (the same set of characters, as with bodies).
    const u8* cursor = in.bytes().data() + in.read_position();
    const u8* const end = in.bytes().data() + in.bytes().size();
    std::unique_ptr<physics3d_detail::CharacterSet::Parsed> characters = m.characters->parse_header(cursor, end);
    if (characters == nullptr) {
        return false;
    }
    in.skip(static_cast<std::size_t>(cursor - (in.bytes().data() + in.read_position())));
    m.characters->prepare_restore(*characters);
    if (!m.system.RestoreState(in) || !m.characters->restore(*characters, in)) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        m.slots[i].user_id = user_ids[i];
    }
    for (u32 i = 0; i < constraint_flags.size(); ++i) {
        m.constraint_slots[i].broken = (constraint_flags[i] & 2u) != 0;
    }
    m.touching = std::move(touching);
    m.dormant = std::move(dormant);
    m.queued.clear();
    m.events.clear();
    m.constraint_events.clear();
    m.characters->clear_events();
    m.collector.clear();
    return true;
}

} // namespace kin
