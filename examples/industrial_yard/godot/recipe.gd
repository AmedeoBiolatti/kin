extends RefCounted
# YRD1: shared packed geometry, floats and poses. No mesh/texture downloads.
var tier=0
var prop_count=0
var material_count=0
var actor_count=0
var smoke_count=0
var world_count=0
var point_count=0
var spot_count=0
var moving_count=0
var source_triangles=0
var recipe_bytes=0
var meshes: Array[ArrayMesh]=[]
var triangles: Array[int]=[]
var materials: Array[StandardMaterial3D]=[]
var route: PackedVector3Array
var world: Array=[]
var props: Array=[]
var actors: Array=[]
var smoke: Array=[]
var pivots: Array[Vector3]=[]
var palettes: Array[Transform3D]=[]
var poses: Array[Transform3D]=[]
var motion: Array[Transform3D]=[]
var lights: Array=[]
var environment: Environment
var robot_vertices: PackedVector3Array
var robot_bones: PackedInt32Array

static func packed(file: FileAccess, type: int, count: int, stride: int) -> Variant:
    var bytes=file.get_buffer(count*stride)
    if bytes.size()!=count*stride:return null
    var h=PackedByteArray();h.resize(8);h.encode_u32(0,type);h.encode_u32(4,count);h.append_array(bytes)
    return bytes_to_var(h)

static func read_transform(file: FileAccess) -> Transform3D:
    var a: PackedFloat32Array=packed(file,TYPE_PACKED_FLOAT32_ARRAY,16,4)
    if a.size()!=16:return Transform3D()
    return Transform3D(Basis(Vector3(a[0],a[1],a[2]),Vector3(a[4],a[5],a[6]),Vector3(a[8],a[9],a[10])),Vector3(a[12],a[13],a[14]))

func objects(file: FileAccess,count: int) -> Array:
    var result: Array=[]
    for _i in count:
        var mesh=file.get_32();var material=file.get_32();var moving=file.get_32();var transform=read_transform(file)
        result.append({"mesh":mesh,"material":material,"moving":moving,"transform":transform})
    return result

func load_recipe(path: String) -> String:
    var f=FileAccess.open(path,FileAccess.READ)
    if f==null or f.get_length()>16*1048576 or f.get_length()<64:return "Yard file missing or exceeds 16 MiB budget"
    recipe_bytes=f.get_length()
    if f.get_32()!=0x31445259 or f.get_32()!=1:return "Invalid yard header"
    tier=f.get_32();prop_count=f.get_32();material_count=f.get_32();actor_count=f.get_32();smoke_count=f.get_32();world_count=f.get_32()
    var mesh_count=f.get_32()
    if f.get_32()!=1440 or f.get_32()!=120 or f.get_32()!=6:return "Invalid yard animation dimensions"
    point_count=f.get_32();spot_count=f.get_32();moving_count=f.get_32()
    if f.get_32()!=1 or tier>2 or prop_count>20000 or material_count<8 or material_count>128 or actor_count>256 or smoke_count>2000 or world_count>512 or mesh_count!=10 or point_count>16 or spot_count>8 or moving_count>prop_count:return "Invalid yard population limits"
    for k in mesh_count:
        var nv=f.get_32();var ni=f.get_32();var nj=f.get_32()
        if nv<1 or nv>10000 or ni<1 or ni>30000 or ni%3 or nj not in [0,6]:return "Invalid yard mesh counts"
        var a=[];a.resize(Mesh.ARRAY_MAX)
        a[Mesh.ARRAY_VERTEX]=packed(f,TYPE_PACKED_VECTOR3_ARRAY,nv,12)
        a[Mesh.ARRAY_NORMAL]=packed(f,TYPE_PACKED_VECTOR3_ARRAY,nv,12)
        a[Mesh.ARRAY_TEX_UV]=packed(f,TYPE_PACKED_VECTOR2_ARRAY,nv,8)
        a[Mesh.ARRAY_TANGENT]=packed(f,TYPE_PACKED_FLOAT32_ARRAY,nv*4,4)
        var indices: PackedInt32Array=packed(f,TYPE_PACKED_INT32_ARRAY,ni,4)
        for i in range(0,ni,3):
            var t=indices[i+1];indices[i+1]=indices[i+2];indices[i+2]=t
        a[Mesh.ARRAY_INDEX]=indices
        if nj:
            a[Mesh.ARRAY_BONES]=packed(f,TYPE_PACKED_INT32_ARRAY,nv*4,4)
            a[Mesh.ARRAY_WEIGHTS]=packed(f,TYPE_PACKED_FLOAT32_ARRAY,nv*4,4)
            robot_vertices=a[Mesh.ARRAY_VERTEX];robot_bones=a[Mesh.ARRAY_BONES]
        var mesh=ArrayMesh.new();mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES,a)
        meshes.append(mesh);triangles.append(ni/3);source_triangles+=ni/3
    var parameters=[]
    for _i in material_count+2:
        var c=f.get_32();var mode=f.get_32();var roughness=f.get_float();var metallic=f.get_float()
        parameters.append([Color(float(c&255)/255,float((c>>8)&255)/255,float((c>>16)&255)/255,float((c>>24)&255)/255),mode,roughness,metallic])
    var maps: Array[ImageTexture]=[]
    for _i in 5:
        var side=f.get_32()
        if side not in [64,128]:return "Invalid yard texture size"
        var bytes=f.get_buffer(side*side*4)
        if bytes.size()!=side*side*4:return "Truncated yard texture"
        maps.append(ImageTexture.create_from_image(Image.create_from_data(side,side,false,Image.FORMAT_RGBA8,bytes)))
    for p in parameters:
        var m=StandardMaterial3D.new();m.albedo_color=p[0];m.roughness=p[2];m.metallic=p[3]
        m.albedo_texture=maps[0];m.metallic_texture=maps[1];m.metallic_texture_channel=BaseMaterial3D.TEXTURE_CHANNEL_BLUE
        m.roughness_texture=maps[1];m.roughness_texture_channel=BaseMaterial3D.TEXTURE_CHANNEL_GREEN
        m.ao_enabled=true;m.ao_texture=maps[1];m.ao_texture_channel=BaseMaterial3D.TEXTURE_CHANNEL_RED
        m.normal_enabled=true;m.normal_texture=maps[2];m.normal_scale=.7
        m.emission_enabled=true;m.emission_texture=maps[3];m.emission=Color(.15,.15,.15)
        m.diffuse_mode=BaseMaterial3D.DIFFUSE_LAMBERT;m.texture_filter=BaseMaterial3D.TEXTURE_FILTER_LINEAR
        if p[1]:
            m.transparency=BaseMaterial3D.TRANSPARENCY_ALPHA;m.cull_mode=BaseMaterial3D.CULL_DISABLED
            if p[1]==2:
                m.shading_mode=BaseMaterial3D.SHADING_MODE_UNSHADED;m.albedo_texture=maps[4];m.normal_enabled=false;m.emission_enabled=false
        materials.append(m)
    if f.get_32()!=128 or f.get_32()!=64:return "Invalid yard sky dimensions"
    var sky_bytes=f.get_buffer(128*64*3*4)
    var panorama=PanoramaSkyMaterial.new();panorama.panorama=ImageTexture.create_from_image(Image.create_from_data(128,64,false,Image.FORMAT_RGBF,sky_bytes))
    var sky=Sky.new();sky.sky_material=panorama;sky.radiance_size=Sky.RADIANCE_SIZE_128;sky.process_mode=Sky.PROCESS_MODE_QUALITY
    environment=Environment.new();environment.background_mode=Environment.BG_COLOR;environment.background_color=Color(15.0/255,22.0/255,35.0/255)
    environment.sky=sky;environment.ambient_light_source=Environment.AMBIENT_SOURCE_SKY;environment.reflected_light_source=Environment.REFLECTION_SOURCE_SKY
    environment.tonemap_mode=Environment.TONE_MAPPER_ACES
    route=packed(f,TYPE_PACKED_VECTOR3_ARRAY,1440*2,12)
    world=objects(f,world_count);props=objects(f,prop_count);actors=objects(f,actor_count);smoke=objects(f,smoke_count)
    for _i in 6:pivots.append(Vector3(f.get_float(),f.get_float(),f.get_float()))
    for _frame in 120:
        for _j in 6:palettes.append(read_transform(f));poses.append(read_transform(f))
    for _frame in 120:motion.append(read_transform(f))
    for _i in point_count+spot_count:
        var p=Vector3(f.get_float(),f.get_float(),f.get_float());var target=Vector3(f.get_float(),f.get_float(),f.get_float());var col=Vector3(f.get_float(),f.get_float(),f.get_float());var radius=f.get_float();var angle=f.get_float();var spot=f.get_32()
        lights.append({"position":p,"target":target,"colour":col,"radius":radius,"angle":angle,"spot":spot})
    if f.get_position()!=f.get_length() or f.get_error()!=OK:return "Truncated or trailing yard data"
    return ""

func moved(o: Dictionary,frame: int,id: int) -> Transform3D:
    var d=motion[(frame+id*17)%120];var t: Transform3D=o.transform
    return Transform3D(d.basis*t.basis,t.origin+d.origin)
