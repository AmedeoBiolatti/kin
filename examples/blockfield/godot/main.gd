extends Node

const WARMUP = 300
const Grove = preload("res://grove.gd")
const Workload = preload("res://render_workload.gd")
var viewport: SubViewport
var camera: Camera3D
var csv: FileAccess
var report_path = ""
var screenshot_path = ""
var snapshot_path = ""
var fov_degrees = 110.0
var measured = 900
var frame = 0
var previous_usec = 0
var chunks = 0
var triangles = 0
var mesh_refs: Array[ArrayMesh] = []
var copies = 1
var triangle_stress = false
var material_kind = "unlit"
var camera_mode = "auto"
var samples = 1
var resolution_scale = 1
var point_count = 0
var spot_count = 0
var shadow_resolution = 1024
var hdr = false
var taa = false
var shadows = false
var local_shadows = false
var normal_map = true
var ao = false
var contact = false
var ibl = false
var dressing_path = ""
var foliage_count = 0
var transparent_count = 0
var dressing
var route_frame = 0
var extent = 0.0
var lights: Array[Light3D] = []

func field_camera() -> bool:
    return camera_mode == "field" or (camera_mode == "auto" and triangle_stress)

func fail(message: String) -> void:
    push_error(message)
    set_process(false)
    get_tree().quit(1)

# Variant packed array headers let the binary snapshot decode in native code.
# This allocates at most one chunk's temporary arrays, never a second world copy.
func packed(file: FileAccess, type: int, count: int, stride: int) -> Variant:
    var payload = file.get_buffer(count * stride)
    if payload.size() != count * stride:
        fail("Truncated snapshot array")
        return null
    var header = PackedByteArray()
    header.resize(8)
    header.encode_u32(0, type)
    header.encode_u32(4, count)
    header.append_array(payload)
    return bytes_to_var(header)

func _ready() -> void:
    var args = OS.get_cmdline_user_args()
    # Apply defaults before parsing explicit overrides, matching the native CLI.
    for j in args.size():
        if args[j] == "--preset":
            if j + 1 >= args.size() or args[j + 1] != "grove":
                fail("Preset must be grove")
                return
            camera_mode = "ground"
            material_kind = "pbr"
            hdr = true
            ibl = true
            shadows = true
            local_shadows = true
            point_count = 4
            spot_count = 1
            foliage_count = 10000
            transparent_count = 512
            fov_degrees = 75
    var metrics_path = ""
    var i = 0
    while i < args.size():
        if i + 1 >= args.size():
            fail("Missing option value")
            return
        match args[i]:
            "--preset": pass
            "--dressing": dressing_path = args[i + 1]
            "--foliage": foliage_count = int(args[i + 1])
            "--transparent": transparent_count = int(args[i + 1])
            "--snapshot": snapshot_path = args[i + 1]
            "--metrics": metrics_path = args[i + 1]
            "--report": report_path = args[i + 1]
            "--screenshot": screenshot_path = args[i + 1]
            "--fov": fov_degrees = float(args[i + 1])
            "--measured": measured = int(args[i + 1])
            "--copies":
                copies = int(args[i + 1])
                triangle_stress = true
            "--material": material_kind = args[i + 1]
            "--camera": camera_mode = args[i + 1]
            "--resolution-scale": resolution_scale = int(args[i + 1])
            "--samples": samples = int(args[i + 1])
            "--lights": point_count = int(args[i + 1])
            "--spots": spot_count = int(args[i + 1])
            "--shadow-resolution": shadow_resolution = int(args[i + 1])
            "--hdr": hdr = args[i + 1] == "1"
            "--taa": taa = args[i + 1] == "1"
            "--shadows": shadows = args[i + 1] == "1"
            "--local-shadows": local_shadows = args[i + 1] == "1"
            "--normal-map": normal_map = args[i + 1] == "1"
            "--ao": ao = args[i + 1] == "1"
            "--contact": contact = args[i + 1] == "1"
            "--environment": ibl = args[i + 1] == "1"
            _:
                fail("Unknown benchmark option: " + args[i])
                return
        i += 2
    if not is_finite(fov_degrees) or fov_degrees < 45 or fov_degrees > 150 or measured < 1 or measured > 6000 or copies < 1 or copies > 100:
        fail("Invalid FOV or frame count")
        return
    if material_kind not in ["unlit", "lit", "pbr"] or camera_mode not in ["auto", "field", "detail", "ground"] or resolution_scale not in [1,2] or samples not in [1,2,4,8] or point_count < 0 or point_count > 16 or spot_count < 0 or spot_count > 16:
        fail("Invalid rendering features")
        return
    if (taa or ao) and RenderingServer.get_current_rendering_method() != "forward_plus":
        fail("TAA/SSAO require Godot Forward+")
        return
    if contact:
        fail("No matched contact-shadow implementation in this Godot workload")
        return
    if RenderingServer.get_current_rendering_method() == "mobile" and (point_count > 8 or spot_count > 8):
        fail("Godot Mobile supports at most 8 lights of each type per mesh")
        return
    if taa and samples != 1:
        fail("Choose TAA or MSAA, not both")
        return
    if shadow_resolution not in [512,1024,2048] or (local_shadows and point_count + spot_count == 0):
        fail("Invalid shadows")
        return
    if material_kind == "unlit" and (point_count or spot_count or shadows or local_shadows or ao):
        fail("Lighting requires a lit or pbr material")
        return
    if foliage_count < 0 or foliage_count > 100000 or transparent_count < 0 or transparent_count > 20000 or ((foliage_count or transparent_count) and camera_mode != "ground"):
        fail("Invalid grove instance count/camera")
        return
    if camera_mode == "ground" and (dressing_path.is_empty() or copies != 1):
        fail("Ground route needs --dressing and one terrain copy")
        return
    var file = FileAccess.open(snapshot_path, FileAccess.READ)
    csv = FileAccess.open(metrics_path, FileAccess.WRITE)
    if file == null or csv == null or file.get_32() != 0x31434642 or file.get_32() != 1:
        fail("Cannot open snapshot/metrics or invalid header")
        return
    chunks = file.get_32()
    var distance = file.get_32()
    extent = 96.0 if camera_mode == "ground" else float((distance + 2) * 16)
    if chunks > 4225 or distance < 1 or distance > 32:
        fail("Invalid snapshot bounds")
        return
    Engine.max_fps = 0
    OS.low_processor_usage_mode = false
    DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    # A continuously rendered offscreen target, like Kin's headless GPU path.
    # Godot --headless uses a dummy renderer and cannot benchmark the GPU.
    RenderingServer.render_loop_enabled = false
    get_window().set_flag(Window.FLAG_NO_FOCUS, true)
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720) * resolution_scale
    viewport.own_world_3d = true
    viewport.render_target_update_mode = SubViewport.UPDATE_DISABLED if triangle_stress else SubViewport.UPDATE_ALWAYS
    viewport.msaa_3d = {1: Viewport.MSAA_DISABLED, 2: Viewport.MSAA_2X, 4: Viewport.MSAA_4X, 8: Viewport.MSAA_8X}[samples]
    viewport.use_taa = taa
    viewport.use_occlusion_culling = false
    add_child(viewport)
    var environment = Environment.new()
    environment.background_mode = Environment.BG_COLOR
    environment.background_color = Color(110.0 / 255, 160.0 / 255, 200.0 / 255)
    environment.tonemap_mode = Environment.TONE_MAPPER_ACES if hdr else Environment.TONE_MAPPER_LINEAR
    if material_kind != "unlit":
        environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
        environment.ambient_light_color = Color(0.12,0.14,0.18).linear_to_srgb()
        environment.ambient_light_energy = 1.0
        environment.ssao_enabled = ao
        environment.ssao_radius = 2.0
        lights = Workload.make_lights(viewport, point_count, spot_count, shadows, local_shadows, shadow_resolution, extent)
    if ibl: Workload.make_environment(environment)
    var world_environment = WorldEnvironment.new()
    world_environment.environment = environment
    viewport.add_child(world_environment)
    camera = Camera3D.new()
    camera.fov = fov_degrees
    camera.keep_aspect = Camera3D.KEEP_HEIGHT
    camera.near = 0.1
    camera.far = 160.0 if camera_mode == "ground" else (distance + 2) * 16.0
    if field_camera():
        camera.fov = 90.0
        camera.far = 3000.0
    viewport.add_child(camera)
    camera.make_current()
    var materials = Workload.make_materials(material_kind, normal_map)
    var source_bytes = 0
    for chunk in chunks:
        var x = file.get_32()
        var z = file.get_32()
        if x >= 0x80000000: x -= 0x100000000
        if z >= 0x80000000: z -= 0x100000000
        var nv = file.get_32()
        var ni = file.get_32()
        source_bytes += nv * 48 + ni * 4
        if nv > 100000 or ni > 300000 or ni % 3 != 0 or source_bytes > 400 * 1048576:
            fail("Invalid chunk size or snapshot exceeds memory budget")
            return
        var arrays = []
        arrays.resize(Mesh.ARRAY_MAX)
        arrays[Mesh.ARRAY_VERTEX] = packed(file, TYPE_PACKED_VECTOR3_ARRAY, nv, 12)
        arrays[Mesh.ARRAY_NORMAL] = packed(file, TYPE_PACKED_VECTOR3_ARRAY, nv, 12)
        var uvs: PackedVector2Array = packed(file, TYPE_PACKED_VECTOR2_ARRAY, nv, 8)
        if material_kind == "pbr":
            for vertex in nv: uvs[vertex] *= 0.125
        arrays[Mesh.ARRAY_TEX_UV] = uvs
        arrays[Mesh.ARRAY_COLOR] = packed(file, TYPE_PACKED_COLOR_ARRAY, nv, 16)
        var indices: PackedInt32Array = packed(file, TYPE_PACKED_INT32_ARRAY, ni, 4)
        # Kin front faces are counter-clockwise; Godot's are clockwise.
        for index in range(0, ni, 3):
            var temp = indices[index + 1]
            indices[index + 1] = indices[index + 2]
            indices[index + 2] = temp
        arrays[Mesh.ARRAY_INDEX] = indices
        var mesh = ArrayMesh.new()
        mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
        mesh.surface_set_material(0, materials[(abs(x) + abs(z)) % 3])
        mesh_refs.append(mesh)
        var instance: GeometryInstance3D
        if triangle_stress:
            var multi = MultiMesh.new()
            multi.transform_format = MultiMesh.TRANSFORM_3D
            multi.mesh = mesh
            multi.instance_count = copies
            var grid = int(ceil(sqrt(float(copies))))
            var scale = 1.0 / grid
            var spacing = float((distance * 2 + 1) * 16)
            for copy_index in copies:
                var tx = (float(copy_index % grid) - float(grid - 1) * 0.5) * spacing
                var tz = (float(copy_index / grid) - float(grid - 1) * 0.5) * spacing
                multi.set_instance_transform(copy_index, Transform3D(Basis.IDENTITY.scaled(Vector3.ONE * scale), Vector3((x * 16 + tx) * scale, 0, (z * 16 + tz) * scale)))
            var multi_instance = MultiMeshInstance3D.new()
            multi_instance.multimesh = multi
            instance = multi_instance
        else:
            var single = MeshInstance3D.new()
            single.mesh = mesh
            single.position = Vector3(x * 16, 0, z * 16)
            instance = single
        instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON if shadows or local_shadows else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
        viewport.add_child(instance)
        triangles += ni / 3
        if chunk % 16 == 15:
            RenderingServer.force_draw(false, 1.0 / 60)
    if file.get_position() != file.get_length():
        fail("Trailing snapshot data")
        return
    file.close()
    if camera_mode == "ground":
        dressing = Grove.new()
        var error = dressing.load_recipe(dressing_path, foliage_count, transparent_count, viewport, shadows or local_shadows, material_kind == "unlit")
        if not error.is_empty():
            fail(error)
            return
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    RenderingServer.viewport_set_measure_render_time(viewport.get_viewport_rid(), true)
    # Match Kin's full-size render-target copy into the final 2D framebuffer.
    var copy = TextureRect.new()
    copy.texture = viewport.get_texture()
    copy.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR
    copy.size = Vector2(1280, 720)
    add_child(copy)
    csv.store_line("frame,frame_interval_ms,godot_cpu_setup_ms,godot_cpu_viewport_ms,godot_gpu_timestamp_ms,draws,triangles,video_memory_bytes,foliage_requested,transparent_requested,route_frame")
    print("snapshot: %d chunks, %d triangles, release=%s" % [chunks, triangles, str(OS.has_feature("release"))])
    previous_usec = Time.get_ticks_usec()

func _process(_delta: float) -> void:
    if viewport == null or csv == null:
        return
    var begin = Time.get_ticks_usec()
    var interval_ms = (begin - previous_usec) / 1000.0
    previous_usec = begin
    var t = float(max(0, frame - WARMUP)) / 60.0
    var travel = fmod(t, 12.0)
    Workload.animate_lights(lights, point_count, t, extent)
    if camera_mode == "ground":
        route_frame = maxi(frame - WARMUP, 0) % Grove.ROUTE_COUNT
        camera.look_at_from_position(dressing.route[route_frame * 2], dressing.route[route_frame * 2 + 1], Vector3.UP)
    elif camera_mode == "detail":
        camera.fov = 60
        camera.position = Vector3(32,46,58)
        camera.look_at(Vector3(8,12,0), Vector3.UP)
    elif field_camera():
        camera.position = Vector3(32 * sin(t * 0.18), 700, 32 * sin(t * 0.12))
        camera.quaternion = Quaternion(Vector3.RIGHT, deg_to_rad(-90))
    else:
        camera.position = Vector3(32 * sin(t * 0.18), 40 + 3 * sin(t * 0.12), -(travel if travel < 6 else 12 - travel) * 8)
        camera.quaternion = Quaternion(Vector3.UP, deg_to_rad(18 * sin(t * 0.21))) * Quaternion(Vector3.RIGHT, deg_to_rad(-28))
    camera.force_update_transform()
    RenderingServer.force_draw(false, 1.0 / 60)
    var rid = viewport.get_viewport_rid()
    var draws = RenderingServer.viewport_get_render_info(rid, RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE, RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME)
    var visible_triangles = RenderingServer.viewport_get_render_info(rid, RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE, RenderingServer.VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME)
    if frame > WARMUP:
        if field_camera() and visible_triangles != triangles * copies:
            fail("Triangle stress did not submit all requested geometry: %d != %d" % [visible_triangles, triangles * copies])
            return
        csv.store_line("%d,%f,%f,%f,%f,%d,%d,%d,%d,%d,%d" % [frame, interval_ms,
            RenderingServer.get_frame_setup_time_cpu(), RenderingServer.viewport_get_measured_render_time_cpu(rid),
            RenderingServer.viewport_get_measured_render_time_gpu(rid), draws, visible_triangles,
            RenderingServer.get_rendering_info(RenderingServer.RENDERING_INFO_VIDEO_MEM_USED), foliage_count, transparent_count, route_frame])
    frame += 1
    if frame > WARMUP + measured:
        csv.close()
        set_process(false)
        if draws <= 0 or visible_triangles <= 0:
            fail("No geometry was rendered; invalid GPU benchmark")
            return
        if not screenshot_path.is_empty():
            var image = viewport.get_texture().get_image()
            if image == null:
                fail("Screenshot capture failed")
                return
            if resolution_scale > 1:
                image.resize(1280, 720, Image.INTERPOLATE_BILINEAR)
            if image.save_png(screenshot_path) != OK:
                fail("Screenshot capture failed")
                return
        var report = {"snapshot": snapshot_path, "resident_chunks": chunks, "resident_triangles": triangles,
            "fov": camera.fov, "warmup_frames": WARMUP, "measured_frames": measured, "material": material_kind, "streaming": false,
            "version": Engine.get_version_info(), "release": OS.has_feature("release"),
            "renderer": RenderingServer.get_current_rendering_method(), "adapter": RenderingServer.get_video_adapter_name(),
            "resolution": [1280, 720], "draws_last": draws, "triangles_last": visible_triangles}
        report["triangle_stress"] = triangle_stress
        report["copies"] = copies
        report["scene_triangles"] = triangles * copies + foliage_count * 6 + transparent_count * 2
        report["mesh_instances"] = chunks * copies + foliage_count + transparent_count
        report["camera_layout"] = "detail" if camera_mode == "detail" else "top_down_disjoint_tiles" if field_camera() else "ground_route" if camera_mode == "ground" else "flight"
        report["point_lights_requested"] = point_count
        report["spot_lights_requested"] = spot_count
        report["render_width"] = viewport.size.x
        report["render_height"] = viewport.size.y
        report["resolution_scale"] = resolution_scale
        report["samples_requested"] = samples
        report["taa_requested"] = taa
        report["hdr"] = hdr
        report["environment"] = ibl
        report["shadows_requested"] = shadows
        report["local_shadows_requested"] = local_shadows
        report["normal_map"] = material_kind == "pbr" and normal_map
        report["ao_requested"] = ao
        report["contact_requested"] = contact
        report["shadow_resolution"] = shadow_resolution
        report["procedural_texture_bytes"] = 65536 if material_kind == "pbr" else 0
        report["dressing"] = dressing_path
        report["foliage_instances"] = foliage_count
        report["transparent_instances"] = transparent_count
        report["camera_route_frame"] = route_frame
        report["extra_texture_bytes"] = 81920 if camera_mode == "ground" else 0
        report["extra_mesh_triangles"] = foliage_count * 6 + transparent_count * 2
        var out = FileAccess.open(report_path, FileAccess.WRITE)
        if out == null:
            fail("Cannot write report")
            return
        out.store_string(JSON.stringify(report, "\t"))
        out.close()
        get_tree().quit()
