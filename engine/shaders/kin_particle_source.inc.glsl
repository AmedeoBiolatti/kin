uint particle_source(uint slot, uint count) {
    uint lo=0u, hi=count;
    while (lo<hi) {
        uint mid=(lo+hi)/2u;
        if (slot<looks[mid].range.y) hi=mid; else lo=mid+1u;
    }
    return lo;
}
