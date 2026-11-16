// kin's terrain surface (kin/render3d/terrain3d.hpp, docs/rendering_3d.md, Terrain): a heightmap terrain drawn as
// CDLOD. Terrain3D draws one flat grid mesh (its quads over the unit square, its vertices at y 0, two more unused at
// y 0 and 1 for its bounds) as one copy a selected quadtree node, placed at the node's corner and lowest height and
// scaled to its size and height range. This surface's offset (every pass runs it: the main pass, the pre-passes, the
// shadows, pick and visibility) morphs each vertex toward the coarser level's grid by its distance from the camera and
// lifts it to the terrain's height there; its fragment shades the terrain's own normal and look.
//
// The maps (Material3D's slots, read as data by texelFetch): the base map holds the heights (RG: a 16-bit height, high
// byte then low; BA: the normal's x and z, 0.5 + 0.5 n), the four others the layers' weights (the metallic-roughness
// slot layers 0-3, the normal slot 4-7, the occlusion slot 8-11, the emissive slot 12-15; read bilinear); the graph
// maps the layers' albedos (sRGB; alpha their height) then their normals (RG; B their roughness). The parameters
// (Terrain3D::material, kin_surface_param):
//   0: the terrain's corner x, z (sample 0, 0), the spacing between samples, the height map's width
//   1: its height, the lowest height, the height a step of the 16 bits is, the node grid's quads a side
//   2: the finest level's range (its nodes drawn within it), where morphing starts (a share of a level's range),
//      the finest node's size, how sharply layers meet by their heights (TerrainLook3D::blend)
//   3: the colour (linear) and roughness of the stock look (without layers)
//   4-7: each layer's tiling (repeats a world unit)
//   8: x the layers' count

// The height at texel `t` (clamped to the map).
float kin_terrain_height_at(ivec2 t) {
    const vec4 p0 = kin_surface_param(0), p1 = kin_surface_param(1);
    t = clamp(t, ivec2(0), ivec2(int(p0.w) - 1, int(p1.x) - 1));
    const vec2 rg = texelFetch(uBase, t, 0).rg * 255.0 + 0.5;
    return p1.y + (floor(rg.r) * 256.0 + floor(rg.g)) * p1.z;
}

// The height at `xz` (world), bilinear between the samples round it.
float kin_terrain_height(vec2 xz) {
    const vec4 p0 = kin_surface_param(0);
    const vec2 s = (xz - p0.xy) / p0.z;
    const ivec2 i = ivec2(floor(s));
    const vec2 f = s - vec2(i);
    const float a = kin_terrain_height_at(i), b = kin_terrain_height_at(i + ivec2(1, 0));
    const float c = kin_terrain_height_at(i + ivec2(0, 1)), d = kin_terrain_height_at(i + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

#ifdef KIN_SURFACE_VERTEX
// A vertex of a node's grid: its place on the grid (its UV times the quads a side), the node's corner and size (its
// copy's placing), its level (from that size), its morph toward the next coarser grid by the camera's distance.
vec3 kin_surface_offset() {
    const vec4 p0 = kin_surface_param(0), p1 = kin_surface_param(1), p2 = kin_surface_param(2);
    const float quads = p1.w;
    const vec3 corner = kin_v_model[3].xyz;
    const float size = length(kin_v_model[0].xyz);
    const float level = max(round(log2(max(size / p2.z, 1.0))), 0.0);
    const float range = p2.x * exp2(level), start = range * p2.y;
    const vec2 grid = kin_v_uv * quads; // 0 .. quads, whole numbers
    const float cell = size / quads;
    vec2 xz = corner.xz + grid * cell;
    // The morph (Strugar's CDLOD): by the distance from the camera to the vertex at the terrain's height there.
    const float here = kin_terrain_height(xz);
    const float distance = length(kin_vs.eye.xyz - vec3(xz.x, here, xz.y));
    const float morph = clamp((distance - start) / max(range - start, 1e-4), 0.0, 1.0);
    const vec2 odd = fract(grid * 0.5) * 2.0; // 1 for the vertices the coarser grid has not
    xz -= odd * morph * cell;
    // (A node past the terrain's far edges: its vertices there fold onto the edge.)
    xz = clamp(xz, p0.xy, p0.xy + (vec2(p0.w, p1.x) - 1.0) * p0.z);
    const float height = kin_terrain_height(xz);
    return vec3(xz.x, height, xz.y) - kin_v_world;
}
#else
// The normal at `xz` (world), bilinear between the samples' (their BA).
vec3 kin_terrain_normal(vec2 xz) {
    const vec4 p0 = kin_surface_param(0), p1 = kin_surface_param(1);
    const vec2 s = (xz - p0.xy) / p0.z;
    const ivec2 i = ivec2(floor(s));
    const vec2 f = s - vec2(i);
    const ivec2 top = ivec2(int(p0.w) - 1, int(p1.x) - 1);
    const vec2 a = texelFetch(uBase, clamp(i, ivec2(0), top), 0).ba;
    const vec2 b = texelFetch(uBase, clamp(i + ivec2(1, 0), ivec2(0), top), 0).ba;
    const vec2 c = texelFetch(uBase, clamp(i + ivec2(0, 1), ivec2(0), top), 0).ba;
    const vec2 d = texelFetch(uBase, clamp(i + ivec2(1, 1), ivec2(0), top), 0).ba;
    const vec2 n = mix(mix(a, b, f.x), mix(c, d, f.x), f.y) * 2.0 - 1.0;
    return normalize(vec3(n.x, sqrt(max(1.0 - dot(n, n), 0.0)), n.y));
}

float kin_terrain_tile(int layer) {
    return kin_surface_param(4 + layer / 4)[layer % 4];
}

void kin_surface(inout KinSurface s) {
    const vec4 look = kin_surface_param(3);
    const vec3 p = kin_position();
    const vec3 n = kin_terrain_normal(p.xz);
    s.normal = n;
    s.alpha = 1.0;
    s.metallic = 0.0;
    s.occlusion = 1.0;
    s.emission = vec3(0.0);
    const int count = int(kin_surface_param(8).x + 0.5);
    if (count == 0) {
        s.base_color = look.rgb;
        s.roughness = clamp(look.a, 0.045, 1.0);
        return;
    }
    // The weights here (their maps' texel centres are the samples).
    const vec4 p0 = kin_surface_param(0), p1 = kin_surface_param(1);
    const vec2 uv = ((p.xz - p0.xy) / p0.z + 0.5) / vec2(p0.w, p1.x);
    const vec4 w4[4] = vec4[4](texture(uMetalRough, uv), texture(uNormalMap, uv), texture(uOcclusion, uv),
                               texture(uEmissive, uv));
    // The four strongest layers here, sampled; then blended by their weights and, as TerrainLook3D::blend says, their
    // heights (the albedo's alpha): the higher layer shows through where two meet.
    int pick[4] = int[4](-1, -1, -1, -1);
    float top[4] = float[4](0.0, 0.0, 0.0, 0.0);
    for (int i = 0; i < count; ++i) {
        float w = w4[i / 4][i % 4];
        int at = i;
        for (int k = 0; k < 4; ++k) {
            if (w > top[k]) {
                const float wt = top[k];
                const int at_k = pick[k];
                top[k] = w;
                pick[k] = at;
                w = wt;
                at = at_k;
            }
        }
    }
    if (pick[0] < 0) {
        pick[0] = 0;
        top[0] = 1.0;
    }
    const float sharp = kin_surface_param(2).w * 16.0;
    vec4 albedos[4], normals[4];
    float highest = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (pick[k] < 0 || top[k] <= 0.0) {
            continue;
        }
        const vec2 t = p.xz * kin_terrain_tile(pick[k]);
        albedos[k] = texture(uGraphMaps, vec3(t, float(pick[k])));
        normals[k] = texture(uGraphMaps, vec3(t, float(count + pick[k])));
        highest = max(highest, albedos[k].a);
    }
    vec3 colour = vec3(0.0), tangent = vec3(0.0);
    float rough = 0.0, total = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (pick[k] < 0 || top[k] <= 0.0) {
            continue;
        }
        const vec4 albedo = albedos[k], normal = normals[k];
        // (Relative to the highest: a layer alone keeps its whole weight.)
        const float f = top[k] * pow((albedo.a + 0.02) / (highest + 0.02), sharp);
        colour += decode(albedo.rgb) * f;
        vec3 m = vec3(normal.rg * 2.0 - 1.0, 0.0);
        m.z = sqrt(max(1.0 - dot(m.xy, m.xy), 0.0));
        tangent += m * f;
        rough += normal.b * f;
        total += f;
    }
    total = max(total, 1e-6);
    s.base_color = colour / total;
    s.roughness = clamp(rough / total, 0.045, 1.0);
    // The layers' normal maps on the terrain's: x along the world's x, y along its z (the maps' v), z out.
    const vec3 tx = normalize(vec3(1.0, 0.0, 0.0) - n * n.x), tz = normalize(vec3(0.0, 0.0, 1.0) - n * n.z);
    const vec3 m = tangent / total;
    s.normal = normalize(tx * m.x + tz * m.y + n * max(m.z, 1e-3));
}
#endif
