// Optional custom-material callback. Define KIN_DIRECT_LIGHT to a function name
// before including kin_material.inc.glsl, then implement that function below it.
// Irradiance is linear, attenuated by range/cone but not by visibility or BRDF.
#ifdef KIN_DIRECT_LIGHT
const int KIN_LIGHT_SUN = 0;
const int KIN_LIGHT_FILL = 1;
const int KIN_LIGHT_POINT = 2;
const int KIN_LIGHT_SPOT = 3;
struct KinLight {
    vec3 direction; // unit vector from the surface toward the light
    vec3 irradiance;
    float visibility; // stock shadow/contact visibility, separately replaceable
    vec3 position;    // world position for points/spots; zero for directionals
    float distance;   // world distance for points/spots; zero for directionals
    int kind;
};
// Return outgoing linear radiance for this direct light. The stock ambient/IBL,
// emission, alpha and output conversion remain separate from this callback.
vec3 KIN_DIRECT_LIGHT(Surface surface, KinLight light);
vec3 kin_direct_light(Surface surface, vec3 direction, vec3 irradiance, float visibility,
                      int kind, vec3 position, float distance) {
    return KIN_DIRECT_LIGHT(surface, KinLight(direction, irradiance, visibility, position, distance, kind));
}
#endif
