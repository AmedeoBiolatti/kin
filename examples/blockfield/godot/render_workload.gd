extends RefCounted
# Counterpart of render_workload.hpp: small procedural maps and a deterministic light rig.

static func make_maps() -> Array[ImageTexture]:
    var buffers: Array[PackedByteArray] = []
    for _map in 4:
        var bytes = PackedByteArray()
        bytes.resize(64 * 64 * 4)
        buffers.append(bytes)
    for y in 64:
        for x in 64:
            var at = (y * 64 + x) * 4
            var checker = ((x / 4) ^ (y / 4)) & 1
            var nx = float(x % 16 - 8) * 0.02
            var ny = float(y % 16 - 8) * 0.02
            var nz = sqrt(1 - nx * nx - ny * ny)
            var glow = (x + y) % 32 < 2
            var values = [[180 + checker * 55, 170 + checker * 55, 155 + checker * 55, 255],
                [180 + checker * 75, 96 + (x % 16) * 10, 255, 255],
                [int(round((nx + 1) * 127.5)), int(round((ny + 1) * 127.5)), int(round((nz + 1) * 127.5)), 255],
                [255 if glow else 0, 60 if glow else 0, 10 if glow else 0, 255]]
            for map_index in 4:
                for c in 4:
                    buffers[map_index][at + c] = values[map_index][c]
    var maps: Array[ImageTexture] = []
    for bytes in buffers:
        maps.append(ImageTexture.create_from_image(Image.create_from_data(64, 64, false, Image.FORMAT_RGBA8, bytes)))
    return maps

static func make_materials(kind: String, normal_map: bool) -> Array[ShaderMaterial]:
    var code = "shader_type spatial; render_mode cull_back, fog_disabled"
    code += ", unshaded;" if kind == "unlit" else ", diffuse_lambert, specular_schlick_ggx;"
    code += """
void vertex() {
    vec3 c = COLOR.rgb;
    COLOR.rgb = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
"""
    if kind == "pbr":
        code = code.replace("void vertex() {", """void vertex() {
    // The voxel UV axes are (normal axis + 1) and (normal axis + 2), modulo 3.
    if (abs(NORMAL.x) > 0.5) { TANGENT = vec3(0,1,0); BINORMAL = vec3(0,0,1); }
    else if (abs(NORMAL.y) > 0.5) { TANGENT = vec3(0,0,1); BINORMAL = vec3(1,0,0); }
    else { TANGENT = vec3(1,0,0); BINORMAL = vec3(0,1,0); }
""")
        code += """
uniform sampler2D base_map : source_color, filter_linear, repeat_enable;
uniform sampler2D orm_map : filter_linear, repeat_enable;
uniform sampler2D emission_map : source_color, filter_linear, repeat_enable;
uniform float metallic_factor;
uniform float roughness_factor;
"""
        if normal_map:
            code += "uniform sampler2D normal_map : filter_linear, repeat_enable;\n"
        code += """
void fragment() {
    ALBEDO = COLOR.rgb * texture(base_map, UV).rgb;
    vec3 orm = texture(orm_map, UV).rgb;
    METALLIC = metallic_factor * orm.b;
    ROUGHNESS = max(0.045, roughness_factor * orm.g);
    AO = orm.r;
    EMISSION = texture(emission_map, UV).rgb * 0.15;
"""
        if normal_map:
            code += "NORMAL_MAP = texture(normal_map, UV).rgb; NORMAL_MAP_DEPTH = 0.7;\n"
        code += "}\n"
    else:
        code += "void fragment() { ALBEDO = COLOR.rgb; ROUGHNESS = 1.0; METALLIC = 0.0; }\n"
    var shader = Shader.new()
    shader.code = code
    var maps: Array[ImageTexture] = make_maps() if kind == "pbr" else []
    var materials: Array[ShaderMaterial] = []
    for i in 3:
        var material = ShaderMaterial.new()
        material.shader = shader
        if kind == "pbr":
            material.set_shader_parameter("base_map", maps[0])
            material.set_shader_parameter("orm_map", maps[1])
            material.set_shader_parameter("emission_map", maps[3])
            material.set_shader_parameter("metallic_factor", [0.0, 0.65, 1.0][i])
            material.set_shader_parameter("roughness_factor", [0.9, 0.45, 0.16][i])
            if normal_map: material.set_shader_parameter("normal_map", maps[2])
        materials.append(material)
    return materials

static func set_light_colour(light: Light3D, linear: Vector3) -> void:
    var energy = max(linear.x, max(linear.y, linear.z))
    light.light_color = Color(linear.x / energy, linear.y / energy, linear.z / energy).linear_to_srgb()
    light.light_energy = energy

static func make_lights(viewport: SubViewport, points: int, spots: int, shadows: bool, local_shadows: bool, resolution: int, extent: float, point_shadows: bool = true) -> Array[Light3D]:
    var sun = DirectionalLight3D.new()
    viewport.add_child(sun)
    sun.look_at_from_position(Vector3.ZERO, Vector3(0.5, -1, -0.35), Vector3.UP)
    set_light_colour(sun, Vector3(0.7, 0.65, 0.55))
    sun.shadow_enabled = shadows
    sun.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
    sun.directional_shadow_max_distance = 900.0
    RenderingServer.directional_shadow_atlas_set_size(resolution, true)
    # Omni shadows occupy two adjacent slots in one quadrant. A single-slot quadrant
    # cannot allocate them. Each atlas slot is resolution squared; Godot renders cube
    # faces at half the slot width before converting to its dual-paraboloid atlas.
    viewport.positional_shadow_atlas_size = resolution * 4
    viewport.positional_shadow_atlas_quad_0 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_4
    viewport.positional_shadow_atlas_quad_1 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_4
    viewport.positional_shadow_atlas_quad_2 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_4
    viewport.positional_shadow_atlas_quad_3 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_4
    if not point_shadows:
        # Spots need one perspective slot, without the adjacent-slot requirement of an omni.
        # Half the atlas width with one slot per quadrant preserves the requested slot size.
        viewport.positional_shadow_atlas_size = resolution * 2
        viewport.positional_shadow_atlas_16_bits = false
        viewport.positional_shadow_atlas_quad_0 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_1
        viewport.positional_shadow_atlas_quad_1 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_1
        viewport.positional_shadow_atlas_quad_2 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_1
        viewport.positional_shadow_atlas_quad_3 = Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_1
    var colours = [Vector3(2400,1200,600),Vector3(600,1200,2400),Vector3(1200,2400,900),Vector3(2400,700,1800)]
    var lights: Array[Light3D] = []
    for i in points:
        var light = OmniLight3D.new()
        set_light_colour(light, colours[i % 4])
        light.omni_range = extent * 2
        light.omni_attenuation = 2.0
        light.omni_shadow_mode = OmniLight3D.SHADOW_CUBE
        light.shadow_enabled = local_shadows and point_shadows and i == 0
        viewport.add_child(light)
        lights.append(light)
    for i in spots:
        var light = SpotLight3D.new()
        set_light_colour(light, colours[(i + 1) % 4] * 2)
        light.spot_range = extent * 2
        light.spot_attenuation = 2.0
        light.spot_angle = 45
        light.shadow_enabled = local_shadows and i < 2
        viewport.add_child(light)
        lights.append(light)
    return lights

static func animate_lights(lights: Array[Light3D], points: int, time: float, extent: float) -> void:
    var spots = lights.size() - points
    for i in lights.size():
        var point = i < points
        var index = i if point else i - points
        var count = points if point else spots
        var a = float(index) * TAU / count + time * (0.18 if point else -0.13)
        var radius = min(extent * (0.45 if point else 0.25), 200.0 if point else 100.0)
        var position = Vector3(radius * cos(a), 80 if point else 100, radius * sin(a))
        lights[i].position = position
        if not point: lights[i].look_at(position + Vector3.DOWN, Vector3.FORWARD)

static func make_environment(environment: Environment) -> void:
    var bytes = PackedByteArray()
    bytes.resize(128 * 64 * 3 * 4)
    for y in 64:
        for x in 128:
            var t = float(y) / 63
            var bright = 4.0 if x >= 76 and x < 96 and y >= 12 and y < 24 else 0.0
            var at = (y * 128 + x) * 12
            bytes.encode_float(at, 0.35 * (1 - t) + 0.05 * t + bright)
            bytes.encode_float(at + 4, 0.5 * (1 - t) + 0.04 * t + bright)
            bytes.encode_float(at + 8, 0.8 * (1 - t) + 0.03 * t + bright)
    var image = Image.create_from_data(128, 64, false, Image.FORMAT_RGBF, bytes)
    var panorama = PanoramaSkyMaterial.new()
    panorama.panorama = ImageTexture.create_from_image(image)
    var sky = Sky.new()
    sky.sky_material = panorama
    sky.radiance_size = Sky.RADIANCE_SIZE_128
    sky.process_mode = Sky.PROCESS_MODE_QUALITY
    environment.sky = sky
    environment.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
    environment.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
