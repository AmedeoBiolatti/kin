#include "world.hpp"
#include <kin/physics3d/world3d.hpp>
#include <cmath>
#include <cstdio>

using namespace blockfield;
int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::fprintf(stderr, "blockfield_tests:%d: %s\n", __LINE__, #c); } } while (0)

int main() {
    CHECK(floor_div(-1, 16) == -1 && floor_div(-16, 16) == -1 && floor_div(-17, 16) == -2);
    CHECK(chunk_of({-1, 20, -17}) == (ChunkKey{-1, -2}));
    const auto cuboid = [](Cell c) { return c.x >= 0 && c.x < 2 && c.y == 0 && c.z == 0 ? Block::Stone : Block::Air; };
    MeshData cube = greedy_mesh(cuboid, {2, 1, 1});
    CHECK(cube.triangle_count() == 12 && cube.vertices.size() == 24 && cube.validate());
    for (std::size_t i = 0; i < cube.indices.size(); i += 3) {
        const auto& a = cube.vertices[cube.indices[i]];
        const auto& b = cube.vertices[cube.indices[i + 1]];
        const auto& c = cube.vertices[cube.indices[i + 2]];
        CHECK(dot(cross(b.position - a.position, c.position - a.position), a.normal) > 0);
    }
    const auto solid = [](Cell c) { return c.x >= 0 && c.x < 32 && c.y >= 0 && c.y < 4 && c.z >= 0 && c.z < 16 ? Block::Stone : Block::Air; };
    const MeshData left = greedy_mesh(solid, {16, 4, 16});
    const MeshData right = greedy_mesh([&](Cell c) { c.x += 16; return solid(c); }, {16, 4, 16});
    CHECK(left.triangle_count() == 10 && right.triangle_count() == 10);
    for (const auto& v : left.vertices) CHECK(!(v.position.x == 16 && v.normal.x == 1));
    for (const auto& v : right.vertices) CHECK(!(v.position.x == 0 && v.normal.x == -1));
    World world(1), same(1), other(7);
    CHECK(world.hash() == same.hash() && world.hash() != other.hash());
    for (int x : {-33, -1, 0, 15, 16, 32}) for (int z : {-17, 0, 16}) {
        CHECK(world.block({x, 0, z}) == Block::Bedrock);
        CHECK(world.block({x, 63, z}) == Block::Air);
        CHECK(world.generated({x, 20, z}) == same.generated({x, 20, z}));
    }
    CHECK(!world.set({0, 0, 0}, Block::Air));
    CHECK(!world.set({0, 64, 0}, Block::Stone));
    CHECK(world.set({15, 50, 0}, Block::Stone));
    auto hit = raycast(world, {12.5f, 50.5f, 0.5f}, {1, 0, 0});
    CHECK(hit && hit->cell == (Cell{15, 50, 0}) && hit->previous == (Cell{14, 50, 0}));
    CHECK(hit && std::abs(hit->distance - 2.5f) < 0.0001f);
    CHECK(world.set({-17, 50, 0}, Block::Wood));
    hit = raycast(world, {-14.5f, 50.5f, 0.5f}, {-1, 0, 0});
    CHECK(hit && hit->cell == (Cell{-17, 50, 0}) && hit->previous == (Cell{-16, 50, 0}));
    CHECK(!raycast(world, {0, 50, 0}, {}, 6));
    auto saved = world.save();
    CHECK(same.restore(saved) && same.hash() == world.hash());
    CHECK(!other.restore(saved));
    saved.pop_back();
    const u64 before = same.hash();
    CHECK(!same.restore(saved) && same.hash() == before);
    auto mesh = build_chunk(world, {0, 0});
    CHECK(mesh.validate() && mesh.triangle_count() > 12);
    CHECK(mesh_bytes(mesh) < 4 * 1024 * 1024);
    // The exact chunk mesh is also the collision surface; rebuilding a mined block opens its collider.
    World3D physics({.max_bodies = 64, .max_body_pairs = 128, .max_contact_constraints = 128,
                     .temp_memory_bytes = 1024 * 1024, .single_threaded = true, .contact_events = false});
    const auto body = [&](const MeshData& data) {
        std::vector<Vec3f> positions;
        for (const auto& v : data.vertices) positions.push_back(v.position);
        return physics.create_body({.shape = Shape3D::triangle_mesh(positions, data.indices), .motion = Body3DMotion::Static});
    };
    Body3D terrain = body(mesh);
    CHECK(terrain.valid());
    CHECK(physics.raycast({15.5f, 55, 0.5f}, {15.5f, 49, 0.5f}).has_value());
    CHECK(world.set({15, 50, 0}, Block::Air));
    physics.destroy_body(terrain);
    terrain = body(build_chunk(world, {0, 0}));
    CHECK(terrain.valid());
    CHECK(!physics.raycast({15.5f, 55, 0.5f}, {15.5f, 49, 0.5f}).has_value());
    CHECK(world.block({15, 50, 0}) == Block::Air);
    // Sampling a chunk after visiting elsewhere preserves edits and has no dependence on chunk load order.
    build_chunk(world, {-2, 1});
    CHECK(world.block({-17, 50, 0}) == Block::Wood && world.block({15, 50, 0}) == Block::Air);
    World capped(1);
    for (std::size_t i = 0; i < max_edits; ++i) CHECK(capped.set({100 + int(i % 128), 50, 100 + int(i / 128)}, Block::Stone));
    CHECK(capped.edits() == max_edits);
    CHECK(!capped.set({500, 50, 500}, Block::Stone));
    CHECK(!capped.set({1000001, 50, 0}, Block::Stone));
    CHECK(capped.set({100, 50, 100}, Block::Air));
    CHECK(capped.set({500, 50, 500}, Block::Stone));
    if (failures) return 1;
    std::puts("blockfield_tests: all passed");
    return 0;
}
