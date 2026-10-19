// kin/render3d on the CPU (no window). InstanceBatch3D: the groups' bounds follow any edit exactly as a measuring again of the group
// would give, what changed_since / changed_ranges_since say of the edits after a revision (also far back, with thousands of edits
// a frame), and the layout revision's counting of groups gaining or losing bounds. ParticleSystem3D: the wind a system's particles
// feel is Wind3D::velocity's field, whatever the number of particles.

#include <kin/render3d/particles3d.hpp>
#include <kin/render3d/scene3d.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <vector>

using namespace kin;

namespace {

struct Lcg {
    u32 s = 99;
    u32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    f32 unit() { return static_cast<f32>(next()) / static_cast<f32>(1u << 24); }
    u32 below(u32 n) { return next() % n; }
};

Mesh test_mesh() {
    struct Fake final : IMeshBackend {};
    return Mesh{std::make_shared<Fake>(), 24, 36, {{-0.5f, -1.0f, -0.25f}, {0.5f, 1.0f, 0.25f}}};
}

bool same(const Bounds3& a, const Bounds3& b) {
    return a.min.x == b.min.x && a.min.y == b.min.y && a.min.z == b.min.z && a.max.x == b.max.x && a.max.y == b.max.y && a.max.z == b.max.z;
}

bool valid(const Bounds3& b) {
    return std::isfinite(b.min.x) && std::isfinite(b.max.x) && std::isfinite(b.min.y) && std::isfinite(b.max.y) &&
           std::isfinite(b.min.z) && std::isfinite(b.max.z) && b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}

MeshInstance random_instance(Lcg& rng, u32 id) {
    // Mixed scales and turns, so that an edit may move the copy that sets a face or one deep inside.
    const f32 scale = 0.3f + rng.unit() * 2.0f;
    const Vec3f at{rng.unit() * 100.0f - 50.0f, rng.unit() * 20.0f, rng.unit() * 100.0f - 50.0f};
    return {Mat4f::trs(at, Quatf::axis_angle({0.0f, 1.0f, 0.0f}, rng.unit() * 360.0f), {scale, scale, scale}), colors::white, id};
}

// Every group's bounds against measuring the group's copies again.
void check_groups(const InstanceBatch3D& batch) {
    for (const InstanceGroup3D& g : batch.groups()) {
        const Bounds3 want = instance_bounds(batch.mesh(), batch.instances().subspan(g.first, g.count));
        assert(same(g.world_bounds, want));
    }
}

// A batch of `count` random copies in groups of `per`, with `gap` unowned copies between neighbours.
InstanceBatch3D make_batch(Lcg& rng, u32 count, u32 per, u32 gap) {
    std::vector<MeshInstance> copies(count);
    std::vector<InstanceGroup3D> groups;
    for (u32 i = 0; i < count; ++i) copies[i] = random_instance(rng, i + 1);
    for (u32 first = 0; first + per <= count; first += per + gap) groups.push_back({first, per});
    return InstanceBatch3D(test_mesh(), std::move(copies), std::move(groups));
}

void test_bounds_follow_edits() {
    Lcg rng;
    InstanceBatch3D batch = make_batch(rng, 4000, 900, 100);
    check_groups(batch);
    // 100 scattered single edits, the groups read after each, then after all of them read once.
    for (int i = 0; i < 100; ++i) {
        const MeshInstance m = random_instance(rng, 5000 + i);
        batch.update(rng.below(4000), {&m, 1});
        check_groups(batch);
    }
    for (int i = 0; i < 500; ++i) {
        const MeshInstance m = random_instance(rng, 6000 + i);
        batch.update(rng.below(4000), {&m, 1});
    }
    check_groups(batch);
    // Runs that span groups and the gaps between them, some of them taken from the batch itself (overlapping its own destination).
    for (int i = 0; i < 40; ++i) {
        const u32 n = 1 + rng.below(1500), first = rng.below(4000 - n + 1);
        std::vector<MeshInstance> run(n);
        for (u32 k = 0; k < n; ++k) run[k] = random_instance(rng, 7000 + k);
        batch.update(first, run);
        if (i % 3 == 0) {
            const u32 m = 1 + rng.below(800), from = rng.below(4000 - m + 1), to = rng.below(4000 - m + 1);
            batch.update(to, batch.instances().subspan(from, m));
        }
        if (i % 4 == 0) check_groups(batch);
    }
    check_groups(batch);
    // Edits that cover whole groups (all their copies new) and ones that cover a group but for a copy at either end.
    for (int i = 0; i < 20; ++i) {
        const auto groups = batch.groups();
        const InstanceGroup3D g = groups[rng.below(static_cast<u32>(groups.size()))];
        const u32 first = g.first - (i % 3 == 1 && g.first > 0 ? 1 : 0), last = g.first + g.count + (i % 3 == 2 && g.first + g.count < 4000 ? 1 : 0);
        std::vector<MeshInstance> run(last - first);
        for (auto& m : run) m = random_instance(rng, 8000);
        batch.update(first, run);
        check_groups(batch);
    }
    // A copy taken off a face of its group's bounds and one put well inside: the bounds shrink, and stay as they were.
    std::vector<MeshInstance> line(10);
    for (u32 i = 0; i < 10; ++i) line[i] = {Mat4f::translation({static_cast<f32>(i), 0.0f, 0.0f}), colors::white, i + 1};
    InstanceBatch3D row(test_mesh(), line, {{0, 10}});
    const Bounds3 before = row.groups()[0].world_bounds;
    const MeshInstance inside{Mat4f::translation({4.5f, 0.0f, 0.0f}), colors::white, 5};
    row.update(4, {&inside, 1});
    assert(same(row.groups()[0].world_bounds, before));
    const MeshInstance away{Mat4f::translation({-20.0f, 0.0f, 0.0f}), colors::white, 1};
    row.update(9, {&away, 1});
    assert(row.groups()[0].world_bounds.max.x < before.max.x - 0.9f && row.groups()[0].world_bounds.min.x < -20.0f);
    check_groups(row);
}

void test_changed_ranges() {
    Lcg rng;
    constexpr u32 count = 5000;
    InstanceBatch3D batch = make_batch(rng, count, 1000, 0);
    // Up to 16 edits behind a reader: exactly their union.
    const u64 start = batch.revision();
    u32 lo = count, hi = 0;
    for (int i = 0; i < 12; ++i) {
        const MeshInstance m = random_instance(rng, 1);
        const u32 at = rng.below(count);
        batch.update(at, {&m, 1});
        lo = std::min(lo, at);
        hi = std::max(hi, at + 1);
    }
    assert(batch.changed_since(start).first == lo && batch.changed_since(start).count == hi - lo);
    // 100 scattered edits: the union, rounded out to 16-instance blocks, not the whole batch.
    const u64 mark = batch.revision();
    std::set<u32> edited;
    lo = count, hi = 0;
    for (int i = 0; i < 100; ++i) {
        const MeshInstance m = random_instance(rng, 2);
        const u32 at = rng.below(count);
        batch.update(at, {&m, 1});
        edited.insert(at);
        lo = std::min(lo, at);
        hi = std::max(hi, at + 1);
    }
    const auto union_range = batch.changed_since(mark);
    assert(union_range.first <= lo && lo - union_range.first < 16 && union_range.first % 16 == 0);
    assert(union_range.first + union_range.count >= hi && union_range.first + union_range.count - hi < 16);
    assert(batch.changed_since(start).first <= lo && batch.changed_since(start).count <= count);
    // The ranges: ascending, disjoint, covering every edit and only blocks that were edited.
    std::vector<InstanceBatch3D::ChangedRange> ranges;
    batch.changed_ranges_since(mark, ranges);
    std::set<u32> blocks;
    for (const u32 at : edited) blocks.insert(at / 16);
    u32 covered = 0, previous_end = 0;
    for (const auto& r : ranges) {
        assert(r.count > 0 && r.first >= previous_end);
        previous_end = r.first + r.count;
        for (u32 i = r.first; i < r.first + r.count; ++i) assert(blocks.contains(i / 16));
        covered += r.count;
    }
    for (const u32 at : edited) assert(std::any_of(ranges.begin(), ranges.end(), [&](const auto& r) { return at >= r.first && at < r.first + r.count; }));
    assert(covered <= edited.size() * 16 && covered < count / 2);
    assert(ranges.size() >= 2); // an edit far from the others is not joined to them
    // A reader nearer the end sees fewer of them; the one at the present sees none; the unknown ones see everything.
    const u64 later = batch.revision();
    const MeshInstance one = random_instance(rng, 3);
    batch.update(4321, {&one, 1});
    batch.update(17, {&one, 1});
    assert(batch.changed_since(later).first == 17 && batch.changed_since(later).count == 4321 - 17 + 1);
    ranges.clear();
    batch.changed_ranges_since(later, ranges);
    assert(ranges.size() == 2 && ranges[0].first == 17 && ranges[0].count == 1 && ranges[1].first == 4321 && ranges[1].count == 1);
    ranges.clear();
    batch.changed_ranges_since(batch.revision(), ranges);
    assert(ranges.empty() && batch.changed_since(batch.revision()).count == 0);
    assert(batch.changed_since(0).count == count && batch.changed_since(batch.revision() + 5).count == count);
    ranges.clear();
    batch.changed_ranges_since(0, ranges);
    assert(ranges.size() == 1 && ranges[0].first == 0 && ranges[0].count == count);
    // Edits that touch or overlap are one range.
    const u64 t = batch.revision();
    const std::vector<MeshInstance> a(5, one), b(5, one);
    batch.update(100, a);
    batch.update(105, b);
    batch.update(103, std::span<const MeshInstance>(a).first(4));
    ranges.clear();
    batch.changed_ranges_since(t, ranges);
    assert(ranges.size() == 1 && ranges[0].first == 100 && ranges[0].count == 10);
    // A reader far behind (more than the exact history and a long run of edits): still the span of what was edited, not the batch.
    const u64 old = batch.revision();
    for (int i = 0; i < 3000; ++i) {
        const u32 at = 2000 + rng.below(100);
        batch.update(at, {&one, 1});
    }
    const auto far = batch.changed_since(old);
    assert(far.first >= 2000 - 16 && far.first + far.count <= 2100 + 16);
    // The batch's own copies see the same history.
    const InstanceBatch3D copy = batch;
    assert(copy.changed_since(old).first == far.first && copy.changed_since(old).count == far.count);
}

void test_layout_revision() {
    Lcg rng;
    constexpr u32 count = 600;
    InstanceBatch3D batch = make_batch(rng, count, 100, 0);
    const InstanceBatch3D copy = batch;
    u64 layout = batch.layout_revision();
    const f32 inf = std::numeric_limits<f32>::infinity();
    MeshInstance broken = random_instance(rng, 1);
    broken.transform.m[0] = inf;
    const MeshInstance fine = random_instance(rng, 2);
    // Two copies without a finite box in a group: it has no bounds until both are mended.
    batch.update(5, {&broken, 1});
    assert(copy.layout_revision() == layout + 1 && !valid(copy.groups()[0].world_bounds));
    batch.update(7, {&broken, 1});
    assert(copy.layout_revision() == layout + 1);
    batch.update(5, {&fine, 1});
    assert(copy.layout_revision() == layout + 1 && !valid(copy.groups()[0].world_bounds));
    batch.update(7, {&fine, 1});
    assert(copy.layout_revision() == layout + 2 && valid(copy.groups()[0].world_bounds));
    check_groups(batch);
    layout = batch.layout_revision();
    // One edit over several groups: the layout moves once for it, whatever number of groups gained or lost their bounds.
    std::vector<MeshInstance> run(250, broken);
    batch.update(50, run);
    assert(batch.layout_revision() == layout + 1);
    assert(!valid(batch.groups()[0].world_bounds) && !valid(batch.groups()[1].world_bounds) && !valid(batch.groups()[2].world_bounds) &&
           valid(batch.groups()[3].world_bounds));
    check_groups(batch);
    // Random edits (some broken) against a model of which groups have bounds: the layout moves exactly when one gained or lost its.
    layout = batch.layout_revision();
    for (int round = 0; round < 400; ++round) {
        std::vector<bool> had;
        for (const auto& g : batch.groups()) had.push_back(valid(g.world_bounds));
        const u32 n = 1 + (rng.below(4) == 0 ? rng.below(150) : 0), first = rng.below(count - n + 1);
        std::vector<MeshInstance> edit(n);
        for (u32 k = 0; k < n; ++k) {
            edit[k] = rng.below(6) == 0 ? broken : random_instance(rng, 9);
            if (rng.below(40) == 0) edit[k].transform.m[13] = std::numeric_limits<f32>::quiet_NaN();
        }
        batch.update(first, edit);
        bool moved = false;
        std::size_t index = 0;
        for (const InstanceGroup3D& g : batch.groups()) {
            const bool has = valid(instance_bounds(batch.mesh(), batch.instances().subspan(g.first, g.count)));
            moved = moved || has != had[index++];
        }
        if (moved) ++layout;
        assert(batch.layout_revision() == layout);
        if (round % 5 == 0) check_groups(batch);
    }
    // Mend everything: all the bounds are back, as measured.
    std::vector<MeshInstance> all(count);
    for (u32 i = 0; i < count; ++i) all[i] = random_instance(rng, i);
    batch.update(0, all);
    for (const auto& g : batch.groups()) assert(valid(g.world_bounds));
    check_groups(batch);
}

// The air a particle is in, read back from one step of the drag's relaxation (v1 = air + (v0 - air) e, with no gravity), against what
// Wind3D::velocity says for its place and the time. A strong drag keeps the read-back well conditioned.
double wind_field_error(const Wind3D& wind, u32 count, f32 influence) {
    ParticleSystem3D system{3};
    ParticleDesc3D& d = system.desc;
    d.shape = ParticleShape3D::Box;
    d.extent = {40.0f, 15.0f, 40.0f};
    d.speed = {0.0f, 0.0f};
    d.lifetime = {500.0f, 500.0f};
    d.drag = 20.0f;
    d.wind_influence = influence;
    d.max_particles = count;
    system.transform = Mat4f::translation({100.0f, 20.0f, -60.0f});
    system.burst(count);
    for (int i = 0; i < 400; ++i) system.update(1.0 / 120.0, &wind); // the particles move off, and the clock with them
    const std::vector<Particle3D> before(system.particles().begin(), system.particles().end());
    const f64 time = system.time();
    system.update(ParticleSystem3D::step, &wind);
    assert(system.count() == before.size());
    const f32 e = std::exp(-d.drag * static_cast<f32>(ParticleSystem3D::step));
    double worst = 0.0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        const Particle3D& a = before[i];
        const Particle3D& b = system.particles()[i];
        const Vec3f seen = (b.velocity - a.velocity * e) / (1.0f - e);
        const Vec3f want = wind.velocity(a.position, time) * influence;
        worst = std::max({worst, static_cast<double>(std::abs(seen.x - want.x)), static_cast<double>(std::abs(seen.y - want.y)),
                          static_cast<double>(std::abs(seen.z - want.z))});
    }
    return worst;
}

void test_particle_wind_field() {
    const Wind3D full{.direction = {1.0f, 0.2f, -0.4f}, .speed = 6.0f, .gust_strength = 0.5f, .gust_period = 8.0f, .gust_length = 45.0f,
                      .turbulence = 0.8f, .turbulence_length = 5.0f};
    // 1000 particles (sampled 32 at a time, the last block part full) and 33 (a block and one), in every combination of the parts.
    for (const u32 count : {1000u, 33u, 1u}) {
        const double worst = wind_field_error(full, count, 0.85f);
        std::cerr << "particle wind: " << count << " particles, the field within " << worst << " m/s of Wind3D::velocity\n";
        assert(worst < 2e-3);
    }
    Wind3D gusts = full; gusts.turbulence = 0.0f;
    Wind3D swirl = full; swirl.speed = 0.0f;
    Wind3D steady = full; steady.gust_strength = 0.0f; steady.turbulence = 0.0f;
    Wind3D slow = full; slow.gust_period = 0.5f; slow.gust_length = 3.0f; slow.turbulence_length = 0.7f; // fine waves: large arguments
    for (const Wind3D& wind : {gusts, swirl, steady, slow}) assert(wind_field_error(wind, 500, 1.0f) < 2e-3);
    assert(wind_field_error(full, 200, 0.0f) < 1e-6); // no influence: the air is still
    const Wind3D none{.direction = {0.0f, 0.0f, 0.0f}, .speed = 5.0f, .gust_strength = 1.0f, .turbulence = 1.0f};
    assert(wind_field_error(none, 100, 1.0f) < 2e-3); // a wind without a direction is still turbulent, as velocity() says
}

} // namespace

int main() {
    test_bounds_follow_edits();
    test_changed_ranges();
    test_layout_revision();
    test_particle_wind_field();
    std::cout << "render3d on the CPU: instance batch bounds exact after scattered, spanning and broken edits, a tight change history past 16 edits, "
                 "particle wind within 1e-3 m/s of Wind3D::velocity\n";
}
