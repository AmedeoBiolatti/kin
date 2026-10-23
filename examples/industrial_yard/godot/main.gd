extends Node
const Recipe=preload("res://recipe.gd")
const WARMUP=300
var data
var options={"recipe":"","metrics":"","report":"","screenshot":"","measured":1440,"resolution-scale":1,"actors":-1,"smoke":-1,"props":-1,"route-offset":0,"animation":1,"motion":1,"glass":1,"structures":1,"local-shadows":1,"taa":0,"ao":0,"contact":0}
var viewport: SubViewport
var camera: Camera3D
var csv: FileAccess
var skeletons: Array[Skeleton3D]=[]
var actor_meshes: Array[MeshInstance3D]=[]
var dynamic: Array=[]
var frame=0
var route_frame=0
var previous_usec=0
var scene_triangles=0
var moving_count=0
var moving_sums=[0.0,0.0,0.0]
var shared_skin: Skin
var skin_validation_error=0.0
var motion_validation_error=0.0
var skin_validations=0

func fail(message: String) -> void:
    push_error(message);set_process(false);get_tree().quit(1)

func instance(o: Dictionary) -> MeshInstance3D:
    var n=MeshInstance3D.new();n.mesh=data.meshes[o.mesh];n.material_override=data.materials[o.material];n.transform=o.transform
    n.cast_shadow=GeometryInstance3D.SHADOW_CASTING_SETTING_ON if o.material<data.material_count else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
    viewport.add_child(n);scene_triangles+=data.triangles[o.mesh];return n

func light_colour(light: Light3D,colour: Vector3) -> void:
    var e=max(colour.x,max(colour.y,colour.z));light.light_color=Color(colour.x/e,colour.y/e,colour.z/e).linear_to_srgb();light.light_energy=e

func _ready() -> void:
    var args=OS.get_cmdline_user_args();var i=0
    while i<args.size():
        var key=args[i].trim_prefix("--")
        if not options.has(key) or i+1>=args.size():fail("Invalid yard option: "+args[i]);return
        options[key]=args[i+1] if key in ["recipe","metrics","report","screenshot"] else int(args[i+1]);i+=2
    if options.recipe.is_empty() or options.metrics.is_empty() or options.measured<1 or options.measured>6000 or options["resolution-scale"] not in [1,2] or options["route-offset"]<0 or options["route-offset"]>=1440 or options.contact:
        fail("Invalid yard options (contact shadows are a Kin-only diagnostic)");return
    if RenderingServer.get_current_rendering_method()!="forward_plus":fail("Yard comparison requires Godot Forward+");return
    data=Recipe.new();var error=data.load_recipe(options.recipe)
    if not error.is_empty():fail(error);return
    for key in ["actors","smoke","props"]:
        var cap=data.actor_count if key=="actors" else data.smoke_count if key=="smoke" else data.prop_count
        if options[key]<0:options[key]=cap
        if options[key]>cap:fail("Yard population exceeds recipe");return
    for key in ["animation","motion","glass","structures","local-shadows","taa","ao"]:
        if options[key] not in [0,1]:fail("Yard booleans require 0 or 1");return
    Engine.max_fps=0;OS.low_processor_usage_mode=false;DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
    RenderingServer.render_loop_enabled=false;get_window().set_flag(Window.FLAG_NO_FOCUS,true)
    viewport=SubViewport.new();viewport.size=Vector2i(1280,720)*options["resolution-scale"];viewport.own_world_3d=true
    viewport.render_target_update_mode=SubViewport.UPDATE_ALWAYS;viewport.use_taa=bool(options.taa);viewport.use_occlusion_culling=false
    viewport.positional_shadow_atlas_16_bits=false
    var subdiv=Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_1 if data.spot_count<=4 else Viewport.SHADOW_ATLAS_QUADRANT_SUBDIV_4
    viewport.positional_shadow_atlas_size=2048 if data.spot_count<=4 else 4096
    viewport.positional_shadow_atlas_quad_0=subdiv;viewport.positional_shadow_atlas_quad_1=subdiv;viewport.positional_shadow_atlas_quad_2=subdiv;viewport.positional_shadow_atlas_quad_3=subdiv
    add_child(viewport)
    data.environment.ssao_enabled=bool(options.ao);data.environment.ssao_radius=2
    var env=WorldEnvironment.new();env.environment=data.environment;viewport.add_child(env)
    var sun=DirectionalLight3D.new();viewport.add_child(sun);sun.look_at_from_position(Vector3.ZERO,Vector3(.5,-1,-.35),Vector3.UP);light_colour(sun,Vector3(.25,.2,.15));sun.shadow_enabled=false
    for d in data.lights:
        var light: Light3D
        if d.spot:
            var spot=SpotLight3D.new();spot.spot_range=d.radius;spot.spot_angle=d.angle;spot.spot_attenuation=2;spot.spot_angle_attenuation=1;spot.shadow_enabled=bool(options["local-shadows"])
            # Godot fixes local-shadow near internally at min(.025, range).
            light=spot
        else:
            var point=OmniLight3D.new();point.omni_range=d.radius;point.omni_attenuation=2;light=point
        light_colour(light,d.colour);viewport.add_child(light);light.position=d.position
        if d.spot:light.look_at(d.target,Vector3.FORWARD)
    if options.structures:
        for o in data.world:
            if options.glass or o.material<data.material_count:instance(o)
    for id in options.props:
        var o=data.props[id]
        if not o.moving:instance(o)
    for mat in data.material_count:
        var ids: PackedInt32Array=[];var mesh=0
        for id in options.props:
            var o=data.props[id]
            if o.moving and o.material==mat:ids.append(id);mesh=o.mesh
        if ids.is_empty():continue
        var multi=MultiMesh.new();multi.transform_format=MultiMesh.TRANSFORM_3D;multi.instance_count=ids.size();multi.mesh=data.meshes[mesh]
        # Use bulk initialization too: set_instance_transform creates a second
        # CPU cache whose deferred AABB refresh can alter shadow cadence.
        var buffer=PackedFloat32Array();buffer.resize(ids.size()*12)
        for n in ids.size():
            var t: Transform3D=data.props[ids[n]].transform
            for row in 3:
                buffer[n*12+row*4]=t.basis.x[row];buffer[n*12+row*4+1]=t.basis.y[row];buffer[n*12+row*4+2]=t.basis.z[row];buffer[n*12+row*4+3]=t.origin[row]
        multi.buffer=buffer
        var node=MultiMeshInstance3D.new();node.multimesh=multi;node.material_override=data.materials[mat];viewport.add_child(node)
        dynamic.append({"multi":multi,"ids":ids,"buffer":buffer});moving_count+=ids.size() if options.motion else 0;scene_triangles+=ids.size()*data.triangles[mesh]
    shared_skin=Skin.new()
    # Native uses the baked palette. Godot forms pose * inverse_bind through its
    # real skeleton path: https://docs.godotengine.org/en/4.7/classes/class_skin.html
    for j in 6:shared_skin.add_bind(j,Transform3D(Basis.IDENTITY,data.pivots[j]).affine_inverse())
    for id in options.actors:
        var o=data.actors[id];var skeleton=Skeleton3D.new();skeleton.name="Robot%d"%id;skeleton.transform=o.transform
        for j in 6:skeleton.add_bone("joint%d"%j);skeleton.set_bone_rest(j,Transform3D(Basis.IDENTITY,data.pivots[j]))
        viewport.add_child(skeleton)
        var node=MeshInstance3D.new();node.mesh=data.meshes[o.mesh];node.skin=shared_skin;node.skeleton=NodePath("..");node.material_override=data.materials[o.material];skeleton.add_child(node)
        skeletons.append(skeleton);actor_meshes.append(node);scene_triangles+=data.triangles[o.mesh]
    for id in options.smoke:instance(data.smoke[id])
    camera=Camera3D.new();camera.fov=70;camera.keep_aspect=Camera3D.KEEP_HEIGHT;camera.near=.1;camera.far=180;viewport.add_child(camera);camera.make_current()
    RenderingServer.viewport_set_measure_render_time(viewport.get_viewport_rid(),true)
    var copy=TextureRect.new();copy.texture=viewport.get_texture();copy.texture_filter=CanvasItem.TEXTURE_FILTER_LINEAR;copy.size=Vector2(1280,720);add_child(copy)
    csv=FileAccess.open(options.metrics,FileAccess.WRITE)
    if csv==null:fail("Cannot write yard CSV");return
    csv.store_line("frame,route_frame,frame_interval_ms,scene_update_ms,godot_cpu_viewport_ms,godot_gpu_timestamp_ms,draws,triangles,local_shadow_draws,local_shadow_triangles,moving_x_sum,moving_y_sum,moving_z_sum")
    previous_usec=Time.get_ticks_usec()

func _process(_delta: float) -> void:
    if viewport==null or csv==null:return
    set_process(false)
    var begin=Time.get_ticks_usec();var interval=(begin-previous_usec)/1000.0;previous_usec=begin
    route_frame=(maxi(frame-WARMUP,0)+options["route-offset"])%1440;moving_sums=[0.0,0.0,0.0]
    for d in dynamic:
        var buf: PackedFloat32Array=d.buffer
        for n in d.ids.size():
            var id=d.ids[n];var t: Transform3D=data.moved(data.props[id],route_frame,id) if options.motion else data.props[id].transform
            for row in (3 if options.motion else 0):
                buf[n*12+row*4]=t.basis.x[row];buf[n*12+row*4+1]=t.basis.y[row];buf[n*12+row*4+2]=t.basis.z[row];buf[n*12+row*4+3]=t.origin[row]
            for axis in 3:moving_sums[axis]+=float(t.origin[axis])
        if options.motion:d.multi.buffer=buf;d.buffer=buf
    for id in skeletons.size():
        if options.animation or frame==0:
            var phase=((route_frame if options.animation else 0)+id*13)%120
            for j in 6:skeletons[id].set_bone_pose(j,data.poses[phase*6+j])
    camera.look_at_from_position(data.route[route_frame*2],data.route[route_frame*2+1],Vector3.UP);camera.force_update_transform()
    var update_ms=(Time.get_ticks_usec()-begin)/1000.0
    # Pose updates are deferred by Skeleton3D. Queue drawing after those updates
    # so the manually forced render consumes this frame's pose, not the previous one.
    call_deferred("draw_frame",interval,update_ms)

func validate_skin() -> bool:
    if skeletons.is_empty():return true
    var node=actor_meshes[0]
    if node.get_node_or_null(node.skeleton)!=skeletons[0] or node.skin.get_bind_count()!=6:return false
    var baked=node.bake_mesh_from_current_skeleton_pose()
    if baked==null:return false
    var vertices: PackedVector3Array=baked.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
    var phase=(route_frame if options.animation else 0)%120
    for i in vertices.size():
        var expected: Vector3=data.palettes[phase*6+data.robot_bones[i*4]]*data.robot_vertices[i]
        skin_validation_error=max(skin_validation_error,vertices[i].distance_to(expected))
    return skin_validation_error<.0001

func validate_motion() -> bool:
    for d in dynamic:
        var uploaded: PackedFloat32Array=d.multi.buffer
        var expected: PackedFloat32Array=d.buffer
        if uploaded.size()!=expected.size():return false
        for i in uploaded.size():motion_validation_error=max(motion_validation_error,abs(uploaded[i]-expected[i]))
    return motion_validation_error<.00001

func draw_frame(interval: float,update_ms: float) -> void:
    RenderingServer.force_draw(false,1.0/60)
    if frame==0 or frame==WARMUP+1:
        if not validate_skin() or not validate_motion():fail("Godot skin palette / uploaded motion mismatch");return
        skin_validations+=1
    var rid=viewport.get_viewport_rid()
    var draws=RenderingServer.viewport_get_render_info(rid,RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE,RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME)
    var tris=RenderingServer.viewport_get_render_info(rid,RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE,RenderingServer.VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME)
    var sd=RenderingServer.viewport_get_render_info(rid,RenderingServer.VIEWPORT_RENDER_INFO_TYPE_SHADOW,RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME)
    var st=RenderingServer.viewport_get_render_info(rid,RenderingServer.VIEWPORT_RENDER_INFO_TYPE_SHADOW,RenderingServer.VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME)
    if frame>WARMUP:csv.store_line("%d,%d,%f,%f,%f,%f,%d,%d,%d,%d,%f,%f,%f"%[frame,route_frame,interval,update_ms,RenderingServer.viewport_get_measured_render_time_cpu(rid),RenderingServer.viewport_get_measured_render_time_gpu(rid),draws,tris,sd,st,moving_sums[0],moving_sums[1],moving_sums[2]])
    frame+=1
    if frame<=WARMUP+options.measured:set_process(true);return
    csv.close()
    if draws==0 or tris==0:fail("Yard rendered no geometry");return
    if not options.screenshot.is_empty():
        var image=viewport.get_texture().get_image()
        if options["resolution-scale"]>1:image.resize(1280,720,Image.INTERPOLATE_BILINEAR)
        if image.save_png(options.screenshot)!=OK:fail("Yard screenshot failed");return
    var report={"recipe":options.recipe,"tier":data.tier,"props":options.props,"actors":options.actors,"smoke":options.smoke,"materials":data.material_count,"structures":bool(options.structures),"glass":bool(options.glass),"motion":bool(options.motion),"animation":bool(options.animation),"shadows":bool(options["local-shadows"]),"taa":bool(options.taa),"ao":bool(options.ao),"contact":false,"point_lights":data.point_count,"spot_lights":data.spot_count,"shadow_resolution":1024,"shadow_near":.025,"shadow_depth_bits":32,"render_width":viewport.size.x,"render_height":viewport.size.y,"route_frame":route_frame,"route_offset":options["route-offset"],"measured_frames":options.measured,"warmup_frames":WARMUP,"moving_props":moving_count,"moving_batches":dynamic.size(),"source_triangles":data.source_triangles,"scene_triangles":scene_triangles,"recipe_bytes":data.recipe_bytes,"moving_x_sum":moving_sums[0],"moving_y_sum":moving_sums[1],"moving_z_sum":moving_sums[2],"skin_validation_error":skin_validation_error,"skin_validations":skin_validations,"motion_validation_error":motion_validation_error,"release":OS.has_feature("release"),"renderer":RenderingServer.get_current_rendering_method(),"positional_shadow_atlas_size":viewport.positional_shadow_atlas_size,"positional_shadow_atlas_16_bits":viewport.positional_shadow_atlas_16_bits}
    if not options.report.is_empty():
        var out=FileAccess.open(options.report,FileAccess.WRITE)
        if out==null:fail("Cannot write yard report");return
        out.store_string(JSON.stringify(report,"  "))
    get_tree().quit()
