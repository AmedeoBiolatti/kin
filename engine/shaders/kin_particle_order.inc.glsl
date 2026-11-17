// Stable far-to-near keys; slot order resolves equal depths. Zero keys are
// invisible and padded slots sort after all real capacity slots.
bool particle_precedes(uvec2 a, uvec2 b) {
    return a.x > b.x || (a.x == b.x && a.y < b.y);
}
shared uvec2 tile[256];
void particle_tile_merge(uint global, uint stride, uint width) {
    uint local = gl_LocalInvocationID.x;
    uvec2 a = tile[local], b = tile[local ^ stride];
    bool high = ((global & width) == 0u) == ((global & stride) == 0u);
    uvec2 value = high == particle_precedes(a, b) ? a : b;
    barrier();
    tile[local] = value;
    barrier();
}
