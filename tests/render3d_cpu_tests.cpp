// kin/render3d on the CPU (no window). InstanceBatch3D: the groups' bounds follow any edit exactly as a measuring again
// of the group would give, what changed_since / changed_ranges_since say of the edits after a revision (also far back,
// with thousands of edits a frame), and the layout revision's counting of groups gaining or losing bounds.
// ParticleSystem3D: the wind a system's particles feel is Wind3D::velocity's field, whatever the number of particles.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <kin/render3d/particles3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <limits>
#include <memory>
#include <set>
#include <vector>

using namespace kin;

namespace {

struct Lcg {
    u32 s = 99;
    u32 next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    f32 unit() { return static_cast<f32>(next()) / static_cast<f32>(1u << 24); }
    u32 below(u32 n) { return next() % n; }
};

Mesh3D test_mesh() {
    struct Fake final : IMeshBackend3D {};
    return Mesh3D{std::make_shared<Fake>(), 24, 36, {{-0.5f, -1.0f, -0.25f}, {0.5f, 1.0f, 0.25f}}};
}

bool same(const Bounds3D& a, const Bounds3D& b) {
    return a.min.x == b.min.x && a.min.y == b.min.y && a.min.z == b.min.z && a.max.x == b.max.x && a.max.y == b.max.y &&
           a.max.z == b.max.z;
}

bool valid(const Bounds3D& b) {
    return std::isfinite(b.min.x) && std::isfinite(b.max.x) && std::isfinite(b.min.y) && std::isfinite(b.max.y) &&
           std::isfinite(b.min.z) && std::isfinite(b.max.z) && b.min.x <= b.max.x && b.min.y <= b.max.y &&
           b.min.z <= b.max.z;
}

MeshInstance3D random_instance(Lcg& rng, u32 id) {
    // Mixed scales and turns, so that an edit may move the copy that sets a face or one deep inside.
    const f32 scale = 0.3f + rng.unit() * 2.0f;
    const Vec3f at{rng.unit() * 100.0f - 50.0f, rng.unit() * 20.0f, rng.unit() * 100.0f - 50.0f};
    return {Mat4f::trs(at, Quatf::axis_angle({0.0f, 1.0f, 0.0f}, rng.unit() * 360.0f), {scale, scale, scale}),
            colors::white, id};
}

// Every group's bounds against measuring the group's copies again.
void check_groups(const InstanceBatch3D& batch) {
    for (const InstanceGroup3D& g : batch.groups()) {
        const Bounds3D want = instance_bounds(batch.mesh(), batch.instances().subspan(g.first, g.count));
        assert(same(g.world_bounds, want));
    }
}

// A batch of `count` random copies in groups of `per`, with `gap` unowned copies between neighbours.
InstanceBatch3D make_batch(Lcg& rng, u32 count, u32 per, u32 gap) {
    std::vector<MeshInstance3D> copies(count);
    std::vector<InstanceGroup3D> groups;
    for (u32 i = 0; i < count; ++i)
        copies[i] = random_instance(rng, i + 1);
    for (u32 first = 0; first + per <= count; first += per + gap)
        groups.push_back({first, per});
    return InstanceBatch3D::create(test_mesh(), std::move(copies), std::move(groups)).value_or({});
}

void test_bounds_follow_edits() {
    Lcg rng;
    InstanceBatch3D batch = make_batch(rng, 4000, 900, 100);
    check_groups(batch);
    // 100 scattered single edits, the groups read after each, then after all of them read once.
    for (int i = 0; i < 100; ++i) {
        const MeshInstance3D m = random_instance(rng, 5000 + i);
        assert(batch.update(rng.below(4000), {&m, 1}));
        check_groups(batch);
    }
    for (int i = 0; i < 500; ++i) {
        const MeshInstance3D m = random_instance(rng, 6000 + i);
        assert(batch.update(rng.below(4000), {&m, 1}));
    }
    check_groups(batch);
    // Runs that span groups and the gaps between them, some of them taken from the batch itself (overlapping its own
    // destination).
    for (int i = 0; i < 40; ++i) {
        const u32 n = 1 + rng.below(1500), first = rng.below(4000 - n + 1);
        std::vector<MeshInstance3D> run(n);
        for (u32 k = 0; k < n; ++k)
            run[k] = random_instance(rng, 7000 + k);
        assert(batch.update(first, run));
        if (i % 3 == 0) {
            const u32 m = 1 + rng.below(800), from = rng.below(4000 - m + 1), to = rng.below(4000 - m + 1);
            assert(batch.update(to, batch.instances().subspan(from, m)));
        }
        if (i % 4 == 0)
            check_groups(batch);
    }
    check_groups(batch);
    // Edits that cover whole groups (all their copies new) and ones that cover a group but for a copy at either end.
    for (int i = 0; i < 20; ++i) {
        const auto groups = batch.groups();
        const InstanceGroup3D g = groups[rng.below(static_cast<u32>(groups.size()))];
        const u32 first = g.first - (i % 3 == 1 && g.first > 0 ? 1 : 0),
                  last = g.first + g.count + (i % 3 == 2 && g.first + g.count < 4000 ? 1 : 0);
        std::vector<MeshInstance3D> run(last - first);
        for (auto& m : run)
            m = random_instance(rng, 8000);
        assert(batch.update(first, run));
        check_groups(batch);
    }
    // A copy taken off a face of its group's bounds and one put well inside: the bounds shrink, and stay as they were.
    std::vector<MeshInstance3D> line(10);
    for (u32 i = 0; i < 10; ++i)
        line[i] = {Mat4f::translation({static_cast<f32>(i), 0.0f, 0.0f}), colors::white, i + 1};
    auto row = InstanceBatch3D::create(test_mesh(), line, {{0, 10}}).value_or({});
    const Bounds3D before = row.groups()[0].world_bounds;
    const MeshInstance3D inside{Mat4f::translation({4.5f, 0.0f, 0.0f}), colors::white, 5};
    assert(row.update(4, {&inside, 1}));
    assert(same(row.groups()[0].world_bounds, before));
    const MeshInstance3D away{Mat4f::translation({-20.0f, 0.0f, 0.0f}), colors::white, 1};
    assert(row.update(9, {&away, 1}));
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
        const MeshInstance3D m = random_instance(rng, 1);
        const u32 at = rng.below(count);
        assert(batch.update(at, {&m, 1}));
        lo = std::min(lo, at);
        hi = std::max(hi, at + 1);
    }
    assert(batch.changed_since(start).first == lo && batch.changed_since(start).count == hi - lo);
    // 100 scattered edits: the union, rounded out to 16-instance blocks, not the whole batch.
    const u64 mark = batch.revision();
    std::set<u32> edited;
    lo = count, hi = 0;
    for (int i = 0; i < 100; ++i) {
        const MeshInstance3D m = random_instance(rng, 2);
        const u32 at = rng.below(count);
        assert(batch.update(at, {&m, 1}));
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
    for (const u32 at : edited)
        blocks.insert(at / 16);
    u32 covered = 0, previous_end = 0;
    for (const auto& r : ranges) {
        assert(r.count > 0 && r.first >= previous_end);
        previous_end = r.first + r.count;
        for (u32 i = r.first; i < r.first + r.count; ++i)
            assert(blocks.contains(i / 16));
        covered += r.count;
    }
    for (const u32 at : edited)
        assert(std::any_of(ranges.begin(), ranges.end(),
                           [&](const auto& r) { return at >= r.first && at < r.first + r.count; }));
    assert(covered <= edited.size() * 16 && covered < count / 2);
    assert(ranges.size() >= 2); // an edit far from the others is not joined to them
    // A reader nearer the end sees fewer of them; the one at the present sees none; the unknown ones see everything.
    const u64 later = batch.revision();
    const MeshInstance3D one = random_instance(rng, 3);
    assert(batch.update(4321, {&one, 1}));
    assert(batch.update(17, {&one, 1}));
    assert(batch.changed_since(later).first == 17 && batch.changed_since(later).count == 4321 - 17 + 1);
    ranges.clear();
    batch.changed_ranges_since(later, ranges);
    assert(ranges.size() == 2 && ranges[0].first == 17 && ranges[0].count == 1 && ranges[1].first == 4321 &&
           ranges[1].count == 1);
    ranges.clear();
    batch.changed_ranges_since(batch.revision(), ranges);
    assert(ranges.empty() && batch.changed_since(batch.revision()).count == 0);
    assert(batch.changed_since(0).count == count && batch.changed_since(batch.revision() + 5).count == count);
    ranges.clear();
    batch.changed_ranges_since(0, ranges);
    assert(ranges.size() == 1 && ranges[0].first == 0 && ranges[0].count == count);
    // Edits that touch or overlap are one range.
    const u64 t = batch.revision();
    const std::vector<MeshInstance3D> a(5, one), b(5, one);
    assert(batch.update(100, a));
    assert(batch.update(105, b));
    assert(batch.update(103, std::span<const MeshInstance3D>(a).first(4)));
    ranges.clear();
    batch.changed_ranges_since(t, ranges);
    assert(ranges.size() == 1 && ranges[0].first == 100 && ranges[0].count == 10);
    // A reader far behind (more than the exact history and a long run of edits): still the span of what was edited, not
    // the batch.
    const u64 old = batch.revision();
    for (int i = 0; i < 3000; ++i) {
        const u32 at = 2000 + rng.below(100);
        assert(batch.update(at, {&one, 1}));
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
    MeshInstance3D broken = random_instance(rng, 1);
    broken.transform.m[0] = inf;
    const MeshInstance3D fine = random_instance(rng, 2);
    // Two copies without a finite box in a group: it has no bounds until both are mended.
    assert(batch.update(5, {&broken, 1}));
    assert(copy.layout_revision() == layout + 1 && !valid(copy.groups()[0].world_bounds));
    assert(batch.update(7, {&broken, 1}));
    assert(copy.layout_revision() == layout + 1);
    assert(batch.update(5, {&fine, 1}));
    assert(copy.layout_revision() == layout + 1 && !valid(copy.groups()[0].world_bounds));
    assert(batch.update(7, {&fine, 1}));
    assert(copy.layout_revision() == layout + 2 && valid(copy.groups()[0].world_bounds));
    check_groups(batch);
    layout = batch.layout_revision();
    // One edit over several groups: the layout moves once for it, whatever number of groups gained or lost their
    // bounds.
    std::vector<MeshInstance3D> run(250, broken);
    assert(batch.update(50, run));
    assert(batch.layout_revision() == layout + 1);
    assert(!valid(batch.groups()[0].world_bounds) && !valid(batch.groups()[1].world_bounds) &&
           !valid(batch.groups()[2].world_bounds) && valid(batch.groups()[3].world_bounds));
    check_groups(batch);
    // Random edits (some broken) against a model of which groups have bounds: the layout moves exactly when one gained
    // or lost its.
    layout = batch.layout_revision();
    for (int round = 0; round < 400; ++round) {
        std::vector<bool> had;
        for (const auto& g : batch.groups())
            had.push_back(valid(g.world_bounds));
        const u32 n = 1 + (rng.below(4) == 0 ? rng.below(150) : 0), first = rng.below(count - n + 1);
        std::vector<MeshInstance3D> edit(n);
        for (u32 k = 0; k < n; ++k) {
            edit[k] = rng.below(6) == 0 ? broken : random_instance(rng, 9);
            if (rng.below(40) == 0)
                edit[k].transform.m[13] = std::numeric_limits<f32>::quiet_NaN();
        }
        assert(batch.update(first, edit));
        bool moved = false;
        std::size_t index = 0;
        for (const InstanceGroup3D& g : batch.groups()) {
            const bool has = valid(instance_bounds(batch.mesh(), batch.instances().subspan(g.first, g.count)));
            moved = moved || has != had[index++];
        }
        if (moved)
            ++layout;
        assert(batch.layout_revision() == layout);
        if (round % 5 == 0)
            check_groups(batch);
    }
    // Mend everything: all the bounds are back, as measured.
    std::vector<MeshInstance3D> all(count);
    for (u32 i = 0; i < count; ++i)
        all[i] = random_instance(rng, i);
    assert(batch.update(0, all));
    for (const auto& g : batch.groups())
        assert(valid(g.world_bounds));
    check_groups(batch);
}

// The air a particle is in, read back from one step of the drag's relaxation (v1 = air + (v0 - air) e, with no
// gravity), against what Wind3D::velocity says for its place and the time. A strong drag keeps the read-back well
// conditioned.
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
    for (int i = 0; i < 400; ++i)
        system.update(1.0 / 120.0, &wind); // the particles move off, and the clock with them
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
        worst =
            std::max({worst, static_cast<double>(std::abs(seen.x - want.x)),
                      static_cast<double>(std::abs(seen.y - want.y)), static_cast<double>(std::abs(seen.z - want.z))});
    }
    return worst;
}

void test_particle_wind_field() {
    const Wind3D full{.direction = {1.0f, 0.2f, -0.4f},
                      .speed = 6.0f,
                      .gust_strength = 0.5f,
                      .gust_period = 8.0f,
                      .gust_length = 45.0f,
                      .turbulence = 0.8f,
                      .turbulence_length = 5.0f};
    // 1000 particles (sampled 32 at a time, the last block part full) and 33 (a block and one), in every combination of
    // the parts.
    for (const u32 count : {1000u, 33u, 1u}) {
        const double worst = wind_field_error(full, count, 0.85f);
        std::cerr << "particle wind: " << count << " particles, the field within " << worst
                  << " m/s of Wind3D::velocity\n";
        assert(worst < 2e-3);
    }
    Wind3D gusts = full;
    gusts.turbulence = 0.0f;
    Wind3D swirl = full;
    swirl.speed = 0.0f;
    Wind3D steady = full;
    steady.gust_strength = 0.0f;
    steady.turbulence = 0.0f;
    Wind3D slow = full;
    slow.gust_period = 0.5f;
    slow.gust_length = 3.0f;
    slow.turbulence_length = 0.7f; // fine waves: large arguments
    for (const Wind3D& wind : {gusts, swirl, steady, slow})
        assert(wind_field_error(wind, 500, 1.0f) < 2e-3);
    assert(wind_field_error(full, 200, 0.0f) < 1e-6); // no influence: the air is still
    const Wind3D none{.direction = {0.0f, 0.0f, 0.0f}, .speed = 5.0f, .gust_strength = 1.0f, .turbulence = 1.0f};
    assert(wind_field_error(none, 100, 1.0f) <
           2e-3); // a wind without a direction is still turbulent, as velocity() says
}

// The catch-up cap: an update runs at most max_catch_up of steps (30), whatever it is given; the rest is dropped and
// counted in skipped_time(), which is not state. A non-finite or negative dt is no time at all. Frames under the cap
// (60 and 144 Hz, ragged ones) still reach one state.
void test_particle_motion_history_and_identity() {
    ParticleSystem3D sampled{7}, plain{7};
    for (auto* system : {&sampled, &plain}) {
        system->desc.rate = 0;
        system->desc.gravity = {};
        system->desc.velocity_min = system->desc.velocity_max = {6, 0, 0};
        system->burst(8);
    }
    std::vector<ParticleSprite3D> old, now, middle;
    sampled.sprites(old);
    sampled.update(1.0 / 60, nullptr, ParticleMotion3D::Capture);
    plain.update(1.0 / 60);
    sampled.sprites(now);
    sampled.sprites(middle, .5f);
    assert(sampled.state_hash() == plain.state_hash() && sampled.save() == plain.save());
    for (std::size_t i = 0; i < old.size(); ++i) {
        assert(length(middle[i].position - lerp(old[i].position, now[i].position, .5f)) < 1e-6f);
        assert(middle[i].id == old[i].id && now[i].id == old[i].id);
    }
    const auto saved = sampled.save();
    const auto before = sampled.state_hash();
    assert(sampled.restore(saved) && sampled.state_hash() == before);
    middle.clear();
    sampled.sprites(middle, 0);
    assert(middle[0].id == now[0].id && length(middle[0].position - old[0].position) < 1e-6f);
    // Rewind one step and resimulate: IDs and the corrected interpolation
    // segment agree with the original timeline, without changing saved bytes.
    sampled.update(1.0 / 60, nullptr, ParticleMotion3D::Capture);
    std::vector<ParticleSprite3D> advanced;
    sampled.sprites(advanced, .5f);
    assert(sampled.restore(saved, ParticleRestore3D::Rollback));
    sampled.update(1.0 / 60, nullptr, ParticleMotion3D::Capture);
    middle.clear();
    sampled.sprites(middle, .5f);
    assert(middle[0].id == advanced[0].id && middle[0].position == advanced[0].position);
    assert(sampled.restore(saved)); // a different snapshot is a discontinuity by default
    middle.clear();
    sampled.sprites(middle);
    assert(middle[0].id != now[0].id);
    const auto restored_id = middle[0].id;
    sampled.clear();
    sampled.burst(1);
    middle.clear();
    sampled.sprites(middle);
    assert(middle[0].id != old[0].id && middle[0].id != restored_id);
    // A first automatic birth interpolates from zero coverage.
    ParticleSystem3D fresh;
    fresh.desc.rate = 120;
    fresh.desc.fade_in = 0;
    fresh.update(ParticleSystem3D::step, nullptr, ParticleMotion3D::Capture);
    middle.clear();
    fresh.sprites(middle, 0);
    assert(!middle.empty() && middle[0].alpha == 0);
}

void test_particle_catch_up_cap() {
    const auto stream = [](u64 seed) {
        ParticleSystem3D s{seed};
        s.desc.rate = 300.0f;
        s.desc.lifetime = {0.5f, 2.0f};
        s.desc.speed = {1.0f, 3.0f};
        s.desc.gravity = {0.0f, -9.8f, 0.0f};
        s.desc.drag = 0.5f;
        s.desc.max_particles = 4000;
        return s;
    };
    const Wind3D wind{.direction = {1.0f, 0.0f, 0.0f}, .speed = 3.0f, .gust_strength = 0.5f, .turbulence = 0.5f};
    static_assert(ParticleSystem3D::max_catch_up == 0.25);
    {
        // A 60 s pause: 30 steps run (a quarter second of simulated time), 59.75 s dropped, the state the one a quarter
        // second gives.
        ParticleSystem3D paused = stream(7), quarter = stream(7);
        paused.update(60.0, &wind);
        quarter.update(0.25, &wind);
        assert(std::abs(paused.time() - 0.25) < 1e-9);
        assert(std::abs(paused.skipped_time() - 59.75) < 1e-9);
        assert(quarter.skipped_time() == 0.0);
        assert(paused.state_hash() == quarter.state_hash() && paused.count() == quarter.count() && paused.count() > 0);
        // Just over the cap: the overflow alone is dropped.
        ParticleSystem3D over = stream(7);
        over.update(0.26, &wind);
        assert(std::abs(over.skipped_time() - 0.01) < 1e-9 && std::abs(over.time() - 0.25) < 1e-9);
        assert(over.state_hash() == quarter.state_hash());
    }
    {
        // Non-finite and negative: ignored, nothing stepped, nothing counted.
        ParticleSystem3D s = stream(7);
        s.update(1.0 / 60.0, &wind);
        const u64 before = s.state_hash();
        const f64 time = s.time();
        for (const f64 bad : {std::numeric_limits<f64>::infinity(), -std::numeric_limits<f64>::infinity(),
                              std::numeric_limits<f64>::quiet_NaN(), -1.0, 0.0}) {
            s.update(bad, &wind);
            assert(s.state_hash() == before && s.time() == time && s.skipped_time() == 0.0);
        }
        // A huge finite dt: one capped update, then it runs on as usual.
        s.update(std::numeric_limits<f64>::max(), &wind);
        assert(std::abs(s.time() - (time + 0.25)) < 1e-9 && std::isfinite(s.skipped_time()) &&
               s.skipped_time() > 1e300);
        s.update(1.0 / 60.0, &wind);
        assert(s.time() > time + 0.25);
        // clear() resets the count.
        s.clear();
        assert(s.skipped_time() == 0.0 && s.time() == 0.0);
    }
    {
        // Frames below the cap: 60 Hz, 144 Hz and ragged frames of 1 to 50 ms give one state, nothing skipped.
        const auto run = [&](const auto& dt_of, f64 total) {
            ParticleSystem3D s = stream(7);
            f64 t = 0.0;
            for (int i = 0; t < total - 1e-12; ++i) {
                const f64 dt = std::min(dt_of(i), total - t);
                s.update(dt, &wind);
                t += dt;
            }
            assert(s.skipped_time() == 0.0);
            return s;
        };
        Lcg lcg{11};
        std::vector<f64> ragged;
        for (int i = 0; i < 200; ++i)
            ragged.push_back(0.001 + 0.049 * static_cast<f64>(lcg.unit()));
        const ParticleSystem3D a = run([](int) { return 1.0 / 60.0; }, 3.0);
        const ParticleSystem3D b = run([](int) { return 1.0 / 144.0; }, 3.0);
        const ParticleSystem3D c = run([&](int i) { return ragged[static_cast<std::size_t>(i) % ragged.size()]; }, 3.0);
        assert(a.count() > 100 && a.state_hash() == b.state_hash() && a.state_hash() == c.state_hash());
    }
    std::cerr << "particles: a 60 s update runs 30 steps and skips 59.75 s; inf, NaN and negative dts are no time\n";
}

// The game clock sums exactly: 216,000 frames of 1/60 s at speed 1 from 20:00 are 21:00 (in f32 `hours` they were
// 20:49), 864,000 of 1/240 s too; setting `hours` by hand is continued from; the hours never read 24.0.
void test_time_of_day_clock_exact() {
    for (const auto [fps, frames] : {std::pair{60, 216000}, std::pair{240, 864000}}) {
        TimeOfDay3D day{.hours = 20.0f, .day_of_year = 100.0f, .speed = 1.0f};
        const f32 dt = 1.0f / static_cast<f32>(fps);
        for (int i = 0; i < frames; ++i)
            kin::advance(day, dt);
        // The f32 dt is not exactly 1/fps: the exact sum of what was given is the target.
        const f64 want = 20.0 + static_cast<f64>(frames) * static_cast<f64>(dt) / 3600.0;
        std::cerr << "time of day: " << frames << " frames at " << fps << " fps from 20.0 h: " << day.hours << " h\n";
        assert(std::abs(static_cast<f64>(day.hours) - want) < 1e-4 && std::abs(want - 21.0) < 1e-5);
        assert(day.day_of_year == 100.0f);
    }
    // Set by the game between frames: the sum starts over from the set value.
    TimeOfDay3D day{.hours = 20.0f, .speed = 1.0f};
    for (int i = 0; i < 6000; ++i)
        kin::advance(day, 1.0f / 60.0f);
    day.hours = 5.0f;
    kin::advance(day, 3600.0f);
    assert(std::abs(day.hours - 6.0f) < 1e-4f);
    // Wrapping, as before: a game hour a second from 23.5 at the year's end.
    TimeOfDay3D wrap{.hours = 23.5f, .day_of_year = 365.0f, .speed = 3600.0f};
    kin::advance(wrap, 1.0f);
    assert(std::abs(wrap.hours - 0.5f) < 1e-4f && wrap.day_of_year == 1.0f);
    kin::advance(wrap, -2.0f);
    assert(std::abs(wrap.hours - 22.5f) < 1e-4f && wrap.day_of_year == 365.0f);
    // A hair under 24 h reads under 24, not 24.0 (f32 would round up); the day has not turned.
    TimeOfDay3D edge{.hours = 23.0f, .day_of_year = 10.0f, .speed = 1.0f};
    kin::advance(edge, 3599.999f);
    assert(edge.hours < 24.0f && edge.hours > 23.9999f && edge.day_of_year == 10.0f);
    kin::advance(edge, 1.0f);
    assert(edge.hours < 0.001f && edge.day_of_year == 11.0f);
    // Very long finite steps stay bounded, in either direction.
    TimeOfDay3D huge{.hours = 8.0f, .speed = 1e30f};
    kin::advance(huge, 1e30f);
    assert(huge.hours >= 0 && huge.hours < 24 && huge.day_of_year >= 1 && huge.day_of_year <= 365);
    kin::advance(huge, -1e30f);
    assert(huge.hours >= 0 && huge.hours < 24 && huge.day_of_year >= 1 && huge.day_of_year <= 365);
    // Not a number: nothing moves.
    TimeOfDay3D still{.hours = 8.0f, .speed = 1.0f};
    kin::advance(still, std::numeric_limits<f32>::quiet_NaN());
    kin::advance(still, std::numeric_limits<f32>::infinity());
    assert(still.hours == 8.0f);
    still.speed = std::numeric_limits<f32>::infinity();
    kin::advance(still, 1.0f);
    assert(still.hours == 8.0f);
}

void test_renderer_owned_sky_lighting() {
    for (f32 hour : {0.0f, 6.0f, 12.0f, 20.0f}) {
        TimeOfDay3D day{.hours = hour, .shadows = true};
        Sky3D sky{.enabled = true};
        const SkyState3D full = evaluate(day, sky);
        assert(full.ambient.r > 0 && full.ground_ambient.b > 0);
        day.compute_ambient = false;
        const SkyState3D fast = evaluate(day, sky);
        assert(fast.ambient.r == 0 && fast.ground_ambient.b == 0);
        assert(fast.sun_direction == full.sun_direction && fast.moon_direction == full.moon_direction);
        assert(fast.sun_color.r == full.sun_color.r && fast.moon_color.b == full.moon_color.b);
        assert(fast.fog_color.g == full.fog_color.g && fast.daylight == full.daylight);
        Lights3D lights;
        lights.ambient = {0.2f, 0.3f, 0.4f};
        lights.ground_ambient = {true, {0.5f, 0.6f, 0.7f}};
        Fog3D fog;
        apply(day, lights, sky, fog);
        assert(lights.ambient.g == 0.3f && lights.ground_ambient.color.b == 0.7f);
        assert(lights.sun.direction == full.sun_direction && fog.colour.g == full.fog_color.g);
        assert(sky.moon_direction == full.moon_direction && sky.moon_light == full.moon_light);
    }

    Sky3D sky{.enabled = true, .light_threshold = 0.01f, .light_interval = 4};
    const Vec3f sun = normalize(Vec3f{0.4f, 1, 0.3f});
    sky_model::AmbientLight3D cache;
    assert(cache.update(sky, sun));
    assert(cache.ambient.r == sky_irradiance(sky, sun, {0, 1, 0}, 16, 8).r);
    assert(cache.ground.b == sky_irradiance(sky, sun, {0, -1, 0}, 16, 8).b);
    const f32 old = cache.ambient.r;
    Sky3D changed = sky;
    changed.intensity *= 2;
    for (int i = 0; i < 3; ++i)
        assert(!cache.update(changed, sun));
    assert(cache.ambient.r == old);
    assert(cache.update(changed, sun));
    assert(cache.ambient.r > old);
    assert(std::abs(cache.ambient.r - sky_irradiance(changed, sun, {0, 1, 0}, 16, 8).r) < 1e-6f);
    for (int i = 0; i < 100; ++i)
        assert(!cache.update(changed, sun));
    changed.intensity += 0.001f;
    assert(!cache.update(changed, sun));
    changed.intensity += 0.1f;
    assert(cache.update(changed, sun)); // Small edits accumulate against the last computed value.
    sky_model::AmbientLight3D other_view;
    assert(other_view.update(sky, sun) && other_view.ambient.r == old);
    assert(!cache.update(changed, sun)); // Another view does not invalidate this view's cache.
    changed.light_interval = 0;
    changed.light_threshold = 0;
    changed.night_color.b += 0.1f;
    assert(cache.update(changed, sun));
}

// The poles: polar day at +90 in June and polar night at -90, the reverse in December (f32 cos(90 degrees) is -4e-8,
// which used to turn the decision round); a latitude past 90 is a pole.
void test_sun_times_at_the_poles() {
    const auto times = [](f32 day, f32 latitude) { return sun_times({.day_of_year = day, .latitude = latitude}); };
    assert(times(172.0f, 90.0f).polar_day && !times(172.0f, 90.0f).polar_night);
    assert(times(172.0f, -90.0f).polar_night && !times(172.0f, -90.0f).polar_day);
    assert(times(355.0f, 90.0f).polar_night && !times(355.0f, 90.0f).polar_day);
    assert(times(355.0f, -90.0f).polar_day && !times(355.0f, -90.0f).polar_night);
    const SunTimes3D day = times(172.0f, 90.0f), night = times(355.0f, 90.0f);
    assert(day.sunrise == 0.0f && day.sunset == 24.0f && night.sunrise == -1.0f && night.sunset == -1.0f);
    // Past the pole: clamped to it.
    assert(times(172.0f, 95.0f).polar_day && times(172.0f, -1000.0f).polar_night);
    // Just short of it: the same answers, by the general formula.
    assert(times(172.0f, 89.999f).polar_day && times(355.0f, 89.999f).polar_night &&
           times(172.0f, -89.999f).polar_night);
    // Away from them, nothing changed: London in June rises and sets.
    const SunTimes3D london = times(172.0f, 51.5f);
    assert(!london.polar_day && !london.polar_night && london.sunrise > 3.5f && london.sunrise < 3.9f);
}

} // namespace

int main() {
    test_bounds_follow_edits();
    test_changed_ranges();
    test_layout_revision();
    test_particle_wind_field();
    test_particle_catch_up_cap();
    test_particle_motion_history_and_identity();
    test_time_of_day_clock_exact();
    test_sun_times_at_the_poles();
    test_renderer_owned_sky_lighting();
    std::cout << "render3d on the CPU: instance batch bounds exact after scattered, spanning and broken edits, a tight "
                 "change history past 16 edits, "
                 "particle wind within 1e-3 m/s of Wind3D::velocity, particle catch-up capped at 30 steps, the game "
                 "clock exact over an hour of frames, polar day and night at the poles\n";
}
