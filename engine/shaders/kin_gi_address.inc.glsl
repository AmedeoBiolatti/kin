// Toroidal GI storage uses positive window dimensions, including fitted, non-power-of-two windows.
ivec3 kin_gi_wrap(ivec3 w, ivec3 s) {
    // Masking also wraps negative world coordinates, without integer divisions in the usual clipmap case.
    const ivec3 mask = s - ivec3(1);
    if (all(equal(s & mask, ivec3(0)))) {
        return w & mask;
    }
    return (w + s * 4096) % s;
}
