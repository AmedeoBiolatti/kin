#version 300 es
// kin's 3D particles in WebGL2: native kin_particle_fragment.inc.glsl and kin_particle_sample.inc.glsl. The fog is
// worked out a fragment (native's: a vertex), as the meshes' is; soft particles read the view's depth texture (drawn
// without it attached, so they also test themselves against it). Premultiplied out, as kin's targets are.
precision highp float;
precision highp int;
in vec2 vUV0;
in vec2 vUV1;
in vec4 vColour;
in vec2 vLocal;
in float vDepth;
in float vMix;
in vec3 vLocalLight;
in vec3 vWorld;
uniform sampler2D uTexture;      // unit 0: the picture
uniform highp sampler2D uDepth;  // unit 1: the view's depth (soft particles)
uniform mat4 uToWorld;           // the view's inverse view-projection
uniform vec2 uTargetInv;         // 1 / the target's size
uniform vec3 uTint;              // the light's tint (linear): mix(1, particle_light_tint, lit)
uniform vec4 uFlags;             // z: blend (0 alpha, 1 additive, 2 premultiplied), w: 1 with a texture
uniform vec4 uDisc;              // x: the procedural disc's softness, y: its ring, z: the soft fade's distance (0: none)
uniform bool uFogged;            // ParticleLook3D::fogged
uniform vec3 uSunDirection, uSunColor;
uniform bool uLinear;            // an HDR view's float picture: linear light, not encoded
uniform bool uCast;              // into a shadow map: only the alpha test (native particle_cast.frag)
uniform float uCutoff;           // ParticleLook3D::shadow_cutoff
uniform float uReceive;          // ParticleLook3D::lit when it receives shadows (0: it doesn't)
out vec4 outColor;
#include "environment.inc.glsl"
#include "shadow_maps.inc.glsl"

vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
float smooth01(float x) {
    x = clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

void main() {
    vec4 tex = vec4(1.0);
    float shape = 1.0;
    if (uFlags.w > 0.5) {
        tex = texture(uTexture, vUV0);
        if (vMix > 0.0) tex = mix(tex, texture(uTexture, vUV1), vMix);
        tex.rgb = decode(tex.rgb);
    } else {
        float r = length(vLocal);
        float s = clamp(uDisc.x, 0.0, 1.0);
        float inner = 1.0 - s;
        float a = r <= inner ? 1.0 : 1.0 - smooth01((r - inner) / max(s, 1e-4));
        if (r > 1.0) a = 0.0;
        if (uDisc.y > 0.0) {
            float width = clamp(uDisc.y, 0.01, 1.0);
            a *= smooth01((r - (1.0 - width)) / width);
        }
        shape = a;
    }
    if (uCast) {
        float covered = tex.a * vColour.a * shape;
        if (covered <= 0.0 || covered < uCutoff) discard;
        outColor = vec4(0.0);
        return;
    }
    float fade = 1.0;
    if (uDisc.z > 0.0) {
        // The surface behind, along the camera's forward, against this fragment's: 0 where they meet; behind it, not
        // drawn (the depth isn't attached here, so the test is this one).
        float d = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
        if (d > 0.0) {
            vec4 w = uToWorld * vec4(gl_FragCoord.xy * uTargetInv * 2.0 - 1.0, d, 1.0);
            float scene = dot(w.xyz / w.w - uEye, uForward);
            if (scene < vDepth) discard;
            fade = smooth01((scene - vDepth) / uDisc.z);
        }
    }
    float k = vColour.a * shape * fade;
    vec3 tint = uTint + vLocalLight;
    // Received shadows (native particle_shadow_tint): the sun's half of the tint through its cascades at this
    // fragment, pushed toward the sun (a volume has no normal). The browser has no other shadow maps.
    if (uReceive > 0.0) tint += 0.5 * uReceive * uSunColor * (sunlight_at(vWorld, uSunDirection) - 1.0);
    vec3 colour = tex.rgb * vColour.rgb * max(tint, vec3(0.0));
    float alpha = tex.a * k;
    if (uFlags.z > 1.5) {
        colour *= k; // premultiplied: the texel is colour * alpha already
    } else {
        colour *= alpha;
    }
    bool additive = uFlags.z > 0.5 && uFlags.z < 1.5;
    if (uFogged && uFogEnabled) {
        vec3 to = vWorld - uEye;
        float dist = length(to);
        vec3 dir = dist > 1e-6 ? to / dist : vec3(0.0, 0.0, -1.0);
        if (uFog.view.x > 0.5) { dir = uForward; dist = dot(to, dir); }
        float opacity = kin_fog_opacity(uFog, kin_fog_optical_depth(uFog, uEye, dir, dist, false));
        if (opacity > 0.0) {
            if (additive) {
                colour *= 1.0 - opacity;
            } else {
                vec3 fog = uFog.colour.w > 0.5 ? fog_sky(dir) : uFog.colour.rgb;
                // (The sun's share comes from the fog volume when it holds the sun.)
                if (!(uFogVolumeAt.x > 0.0 && uFogVolumeAt.y > 0.5))
                    fog += kin_fog_sun_light(uFog, dir, uSunDirection, uSunColor);
                colour = mix(colour, fog * alpha, opacity);
            }
        }
    }
    if (!uLinear) {
        vec3 straight = alpha > 1e-4 ? colour / alpha : colour;
        straight = encode(straight);
        colour = alpha > 1e-4 ? straight * alpha : straight;
    }
    outColor = vec4(colour, additive ? 0.0 : alpha);
}
