#version 450 core
// Auto exposure (ViewOptions3D::auto_exposure), its second half: one workgroup reads the histogram exposure_histogram.comp
// made, averages the log2 luminance of the pixels between the `low` and `high` shares of it (the darkest and the
// brightest left out: a lamp or a patch of sky does not darken the whole picture), turns that into the exposure that
// brings the average to `key`, clamped to the allowed range, and moves the view's exposure toward it, faster or slower
// as it brightens or darkens. Then it clears the histogram for the next frame. Mode 0 only clears (a new view, whose
// buffer holds nothing yet).
layout(local_size_x = 128) in;

const uint BINS = 128;

layout(std430, set = 1, binding = 0) buffer Exposure {
    uint bins[BINS];
    float ev;
    float exposure;
    float target;
    float pad;
} state;
layout(std140, set = 2, binding = 0) uniform Params {
    vec4 a; // x: the histogram's lowest log2 luminance, y: bins per stop, z: low share, w: high share
    vec4 b; // x: log2 of the key, y: the least exposure (stops), z: the most, w: 1 to start at the target (no history)
    vec4 c; // x: the share of the way to go this frame when brightening (1 - exp(-dt * speed_up)), y: when darkening,
            // z: mode (0 clear only, 1 adapt and clear), w: compensation (stops, added to the target)
} P;

shared uint counts[BINS];

void main() {
    const uint index = gl_LocalInvocationIndex;
    counts[index] = P.c.z > 0.5 ? state.bins[index] : 0u;
    barrier();
    if (index == 0u && P.c.z > 0.5) {
        uint total = 0u;
        for (uint i = 1u; i < BINS; ++i) {
            total += counts[i];
        }
        float target = state.target;
        if (total > 0u) {
            const float low = float(total) * P.a.z, high = float(total) * max(P.a.w, P.a.z);
            float below = 0.0, sum = 0.0, weight = 0.0;
            for (uint i = 1u; i < BINS; ++i) {
                const float n = float(counts[i]);
                // The part of this bin's run of the sorted pixels that is between low and high.
                const float kept = max(min(below + n, high) - max(below, low), 0.0);
                const float centre = P.a.x + (float(i) + 0.5) / P.a.y;
                sum += kept * centre;
                weight += kept;
                below += n;
            }
            if (weight > 0.0) {
                target = clamp(P.b.x - sum / weight + P.c.w, P.b.y, P.b.z);
            }
        } else if (P.b.w > 0.5) {
            target = clamp(P.c.w, P.b.y, P.b.z); // nothing to meter yet: the exposure as given
        }
        float ev = state.ev;
        if (P.b.w > 0.5 || isnan(ev) || isinf(ev)) {
            ev = target;
        } else {
            ev += (target - ev) * (target > ev ? P.c.x : P.c.y);
        }
        state.ev = ev;
        state.exposure = exp2(ev);
        state.target = target;
    }
    if (P.c.z < 0.5 && index == 0u) {
        state.ev = clamp(P.c.w, P.b.y, P.b.z);
        state.exposure = exp2(state.ev);
        state.target = state.ev;
        state.pad = 0.0;
    }
    state.bins[index] = 0u;
}
