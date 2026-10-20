    // The picture, or a soft disc (with a ring, if asked): alpha 0 outside the disc's edge.
    vec4 tex = vec4(1.0);
    float shape = 1.0;
    if (frag.flags.w > 0.5) {
        tex = texture(uTexture, vUV0);
        if (vMix > 0.0) {
            tex = mix(tex, texture(uTexture, vUV1), vMix);
        }
        if (frag.flags.y > 0.5) {
            tex.rgb = decode(tex.rgb);
        }
    } else {
        const float r = length(vLocal);
        const float s = clamp(frag.disc.x, 0.0, 1.0);
        const float inner = 1.0 - s;
        float a = r <= inner ? 1.0 : 1.0 - smooth01((r - inner) / max(s, 1e-4));
        if (r > 1.0) {
            a = 0.0;
        }
        if (frag.disc.y > 0.0) {
            const float width = clamp(frag.disc.y, 0.01, 1.0);
            a *= smooth01((r - (1.0 - width)) / width);
        }
        shape = a;
    }
