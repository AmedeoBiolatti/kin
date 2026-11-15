// The sun's cascades (depth24 maps, manual 3 x 3 PCF): what a lit point at `p` (pushed off along `n`) receives of the
// sun, for the mesh shader (shadow.inc.glsl) and the particles' (a volume has no normal: pushed toward the sun).
uniform highp sampler2DArray uShadow;
uniform int uShadowCount;
uniform mat4 uShadowMatrix[4];
uniform vec4 uShadowEnds, uShadowBlend, uShadowBias, uShadowOffset;
uniform float uShadowTexel, uShadowStrength;
float shadow_cascade_at(int layer,vec3 world,vec3 n) {
    vec4 clip=uShadowMatrix[layer]*vec4(world+n*uShadowOffset[layer],1.0);
    vec3 p=clip.xyz/clip.w;
    vec2 uv=p.xy*0.5+0.5;
    if (p.z<0.0 || p.z>1.0 || any(lessThan(uv,vec2(0))) || any(greaterThan(uv,vec2(1)))) return 1.0;
    float seen=0.0;
    for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) {
        float depth=texture(uShadow,vec3(uv+vec2(float(x),float(y))*uShadowTexel,float(layer))).r;
        seen += p.z+uShadowBias[layer]>=depth ? 1.0 : 0.0;
    }
    return seen/9.0;
}
// The sun's share at `world`, its cascade by view depth (uEye, uForward), blended into the next, times the strength.
float sunlight_at(vec3 world,vec3 n) {
    if (uShadowCount==0) return 1.0;
    float depth=dot(world-uEye,uForward);
    if (depth>uShadowEnds[uShadowCount-1]) return 1.0;
    int layer=0;
    for (int i=0;i<3;++i) if (i<uShadowCount-1 && depth>uShadowEnds[i]) layer=i+1;
    float seen=shadow_cascade_at(layer,world,n);
    if (layer<uShadowCount-1 && depth>uShadowBlend[layer]) {
        float w=smoothstep(uShadowBlend[layer],uShadowEnds[layer],depth);
        seen=mix(seen,shadow_cascade_at(layer+1,world,n),w);
    }
    return mix(1.0,seen,uShadowStrength);
}

// Point and spot lights' maps (native kin_shadows.inc.glsl's local_light): one depth layer a map of uLocalShadow (a
// spot's, or a face of a point light's cube), drawn into its corner of a.x of the layer's side. uShadowData's row
// `layer` holds the map's view-projection (texels 0-3, columns), a (x: its share of the layer, y: a texel's width per
// unit of distance, z: near, w: far) and b (x: bias texels, y: normal offset texels, z: softness texels, w: the
// distance its shadow fades out by); the moon's cascades are rows too (uMoonRow + k: their matrix, then x depth bias,
// y normal offset). Reverse Z: a perspective depth(d) = near (far / d - 1) / (far - near).
uniform highp sampler2DArray uLocalShadow;
uniform highp sampler2D uShadowData;
uniform float uLocalTexel;              // 1 / the local array's side
uniform vec4 uPointShadow[4];           // the mesh lights' (uPoints) first cube layer, -1: none
uniform highp sampler2DArray uMoonShadow;
uniform int uMoonCount, uMoonRow;       // the moon's cascades (0: none) and their first row
uniform vec4 uMoonEnds, uMoonBlend;
uniform float uMoonTexel, uMoonStrength;
mat4 shadow_matrix(int row) {
    return mat4(texelFetch(uShadowData, ivec2(0, row), 0), texelFetch(uShadowData, ivec2(1, row), 0),
                texelFetch(uShadowData, ivec2(2, row), 0), texelFetch(uShadowData, ivec2(3, row), 0));
}
// The share of map `layer` that lets the light through at `world` (pushed off along `offset_normal`), toward the light
// `l`: nine comparisons (`taps` 9) or one (`taps` 1: the fog's air); -1 outside the map's view.
float local_light_at(int layer, vec3 world, vec3 offset_normal, vec3 l, int taps) {
    mat4 m = shadow_matrix(layer);
    vec4 a = texelFetch(uShadowData, ivec2(4, layer), 0), b = texelFetch(uShadowData, ivec2(5, layer), 0);
    float texel_world = (m * vec4(world, 1.0)).w * a.y;
    vec4 clip = m * vec4(world + offset_normal * (b.y * texel_world), 1.0);
    if (clip.w <= a.z) return -1.0;
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || clip.w > a.w) return -1.0;
    float n_l = clamp(dot(offset_normal, l), 0.0, 1.0);
    float slope = taps == 1 ? 0.0 : min(sqrt(max(1.0 - n_l * n_l, 0.0)) / max(n_l, 0.05), 12.0);
    float d = max(clip.w - texel_world * b.x * (1.0 + slope), a.z);
    float ref = a.z * (a.w / d - 1.0) / (a.w - a.z);
    vec2 base = uv * a.x;
    vec2 lo = vec2(0.5 * uLocalTexel), hi = vec2(a.x - 0.5 * uLocalTexel);
    if (taps == 1) return ref >= texture(uLocalShadow, vec3(clamp(base, lo, hi), float(layer))).r ? 1.0 : 0.0;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 q = clamp(base + vec2(float(x), float(y)) * (b.z * uLocalTexel), lo, hi);
            lit += ref >= texture(uLocalShadow, vec3(q, float(layer))).r ? 1.0 : 0.0;
        }
    return lit / 9.0;
}
// Its share of the light, the shadow faded out toward b.w from the light (1: no shadow, or not in the map's view).
float local_shadow_at(int layer, vec3 world, vec3 offset_normal, vec3 l, float distance, int taps) {
    float lit = local_light_at(layer, world, offset_normal, l, taps);
    if (lit < 0.0) return 1.0;
    float end = texelFetch(uShadowData, ivec2(5, layer), 0).w;
    return mix(lit, 1.0, smoothstep(0.8 * end, end, distance));
}
// A point light's: the cube face its direction from the light falls in, from the cube's `first` layer (-1: none).
float point_shadow_at(float first, vec3 world, vec3 offset_normal, vec3 toward, float distance, int taps) {
    if (first < 0.0) return 1.0;
    vec3 from_light = -toward, m = abs(from_light);
    int face = m.x >= m.y && m.x >= m.z ? (from_light.x < 0.0 ? 1 : 0)
             : m.y >= m.z               ? (from_light.y < 0.0 ? 3 : 2)
                                        : (from_light.z < 0.0 ? 5 : 4);
    return local_shadow_at(int(first) + face, world, offset_normal, toward, distance, taps);
}
float point_shadow_layer(int i) {
    return uPointShadow[i / 4][i % 4];
}
// One of the moon's cascades; -1 outside its map.
float moon_cascade_at(int k, vec3 world, vec3 offset_normal, int taps) {
    vec4 bias = texelFetch(uShadowData, ivec2(4, uMoonRow + k), 0);
    vec4 clip = shadow_matrix(uMoonRow + k) * vec4(world + offset_normal * bias.y, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || ndc.z < 0.0 || ndc.z > 1.0) return -1.0;
    float ref = ndc.z + bias.x;
    if (taps == 1) return ref >= texture(uMoonShadow, vec3(uv, float(k))).r ? 1.0 : 0.0;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            lit += ref >= texture(uMoonShadow, vec3(uv + vec2(float(x), float(y)) * uMoonTexel, float(k))).r ? 1.0 : 0.0;
    return lit / 9.0;
}
// How much of the moon (the fill light) reaches `world`: its cascade by view depth, faded into the next, by strength.
float moonlight_at(vec3 world, vec3 offset_normal, int taps) {
    if (uMoonCount < 1) return 1.0;
    float depth = dot(world - uEye, uForward);
    int k = 0;
    for (int i = 0; i < 3; ++i) if (i < uMoonCount - 1 && depth > uMoonEnds[i]) k = i + 1;
    if (uMoonCount > 1 && depth > uMoonEnds[uMoonCount - 1]) return 1.0;
    float lit = moon_cascade_at(k, world, offset_normal, taps);
    if (k < uMoonCount - 1 && depth > uMoonBlend[k]) {
        float t = clamp((depth - uMoonBlend[k]) / max(uMoonEnds[k] - uMoonBlend[k], 1e-4), 0.0, 1.0);
        float next = moon_cascade_at(k + 1, world, offset_normal, taps);
        lit = lit < 0.0 ? next : next < 0.0 ? lit : mix(lit, next, t);
    }
    return lit < 0.0 ? 1.0 : mix(1.0, lit, uMoonStrength);
}
