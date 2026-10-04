#pragma once

#include <kin/core/types.hpp>

#include <memory>
#include <span>
#include <vector>

namespace kin {

enum class PhysicsBodyType {
    Static,
    Dynamic,
    Kinematic,
};

enum class PhysicsShapeType {
    Box,
    Circle,
};

enum class PhysicsContactEventType {
    Begin,
    End,
};

enum class PhysicsDebugShapeType {
    Polygon,
    Circle,
};

struct PhysicsBody {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(PhysicsBody, PhysicsBody) = default;
};

struct PhysicsFixture {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(PhysicsFixture, PhysicsFixture) = default;
};

struct PhysicsWorldDef {
    Vec2f gravity{};
};

struct PhysicsBodyDef {
    PhysicsBodyType type = PhysicsBodyType::Static;
    Vec2f position{};
    f32 rotation = 0.0f; // radians, as Box2D (Transform2D is degrees; the ECS sync converts)
    Vec2f linear_velocity{};
    f32 angular_velocity = 0.0f;
    f32 linear_damping = 0.0f;
    f32 angular_damping = 0.0f;
    f32 gravity_scale = 1.0f;
    bool fixed_rotation = false;
    u64 user_id = 0;
};

struct PhysicsShape {
    PhysicsShapeType type = PhysicsShapeType::Box;
    Vec2f half_extents{0.5f, 0.5f};
    Vec2f offset{};
    f32 radius = 0.5f;

    static PhysicsShape box(Vec2f half_extents, Vec2f offset = {});
    static PhysicsShape circle(f32 radius, Vec2f offset = {});
};

struct PhysicsFixtureDef {
    PhysicsShape shape{};
    f32 density = 1.0f;
    f32 friction = 0.2f;
    f32 restitution = 0.0f;
    bool sensor = false;
    u16 category_bits = 0x0001;
    u16 mask_bits = 0xFFFF;
    u64 tag = 0;
};

struct PhysicsFilter {
    u16 category_bits = 0xFFFF;
    u16 mask_bits = 0xFFFF;
    bool include_sensors = true;
};

struct PhysicsContactEvent {
    PhysicsContactEventType type = PhysicsContactEventType::Begin;
    PhysicsBody body_a{};
    PhysicsBody body_b{};
    PhysicsFixture fixture_a{};
    PhysicsFixture fixture_b{};
    bool sensor = false;
    bool has_point = false;
    Vec2f point{};
    Vec2f normal{};
};

struct PhysicsContact {
    PhysicsBody body_a{};
    PhysicsBody body_b{};
    PhysicsFixture fixture_a{};
    PhysicsFixture fixture_b{};
    bool sensor = false;
};

struct PhysicsRaycastHit {
    PhysicsBody body{};
    PhysicsFixture fixture{};
    Vec2f point{};
    Vec2f normal{};
    f32 fraction = 0.0f;
};

struct PhysicsDebugShape {
    PhysicsDebugShapeType type = PhysicsDebugShapeType::Polygon;
    PhysicsBody body{};
    PhysicsFixture fixture{};
    PhysicsBodyType body_type = PhysicsBodyType::Static;
    bool sensor = false;
    bool awake = false;
    std::vector<Vec2f> points;
    Vec2f center{};
    f32 radius = 0.0f;
};

class PhysicsWorld {
public:
    explicit PhysicsWorld(PhysicsWorldDef def = {});
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;

    PhysicsBody create_body(const PhysicsBodyDef& def);
    void destroy_body(PhysicsBody body);
    bool is_alive(PhysicsBody body) const;

    PhysicsFixture add_fixture(PhysicsBody body, const PhysicsFixtureDef& def);
    void destroy_fixture(PhysicsFixture fixture);
    bool is_alive(PhysicsFixture fixture) const;
    std::vector<PhysicsFixture> fixtures(PhysicsBody body) const;

    u64 user_id(PhysicsBody body) const;
    u64 fixture_tag(PhysicsFixture fixture) const;

    // Angles in radians.
    void set_transform(PhysicsBody body, Vec2f position, f32 rotation);
    Vec2f position(PhysicsBody body) const;
    f32 rotation(PhysicsBody body) const;

    void set_linear_velocity(PhysicsBody body, Vec2f velocity);
    Vec2f linear_velocity(PhysicsBody body) const;
    void apply_force(PhysicsBody body, Vec2f force);
    void apply_impulse(PhysicsBody body, Vec2f impulse);

    void step(f32 dt, i32 velocity_iterations = 8, i32 position_iterations = 3);

    std::span<const PhysicsContactEvent> contacts() const;
    std::span<const PhysicsContact> current_contacts() const;
    void clear_contacts();

    std::vector<PhysicsFixture> query_point(Vec2f point, PhysicsFilter filter = {}) const;
    std::vector<PhysicsFixture> query_aabb(Rectf rect, PhysicsFilter filter = {}) const;
    std::vector<PhysicsRaycastHit> raycast(Vec2f from, Vec2f to, PhysicsFilter filter = {}) const;
    std::vector<PhysicsDebugShape> debug_shapes(PhysicsFilter filter = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
