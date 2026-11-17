layout(set = 1, binding = 0) uniform ParticleDraw {
    mat4 view_projection; // jittered: where the pixel is drawn
    vec4 right;           // xyz: the camera's right,
    vec4 up;              //      up
    vec4 eye;             // xyz: where it is, w: 1 for an orthographic camera
    vec4 forward;         // xyz: where it looks
    vec4 axis;            // xyz: the Axis and Flat facings' axis (unit), w: the facing (0 camera, 1 stretched, 2 axis, 3 flat)
    vec4 atlas;           // x: columns, y: rows, z: frames, w: 1 to mix two frames
    vec4 stretch;         // x: seconds a streak spans, y: its least length
    uvec4 first;          // x: this batch's first sprite, y: 1 when fogged
    vec4 sun_direction;   // xyz: toward the sun
    vec4 sun_light;       // rgb: its light
    KinFog fog;
    KinSky sky;
    vec4 clip; // Camera3D::clip_plane: drawn where dot(xyz, world) + w >= 0 (0, 0, 0, 1: everywhere)
} draw;
layout(set = 1, binding = 1) uniform FogTable {
    vec4 rows[KIN_FOG_ROWS * 4];
} fog_table;
KIN_FOG_TABLE_READER(particle_fog_sky, draw.sky, fog_table.rows)
