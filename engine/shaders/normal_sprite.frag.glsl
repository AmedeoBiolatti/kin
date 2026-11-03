#version 450 core
layout(location=0) in struct { vec4 Color; vec2 UV; } In;
layout(location=2) in vec4 World;
layout(location=0) out vec4 fColor;
layout(set=2,binding=0) uniform sampler2D Albedo;
layout(set=2,binding=1) uniform sampler2D Normal;
layout(set=2,binding=2) uniform sampler2D Specular;
layout(std430,set=2,binding=3) readonly buffer Lighting { vec4 data[]; };
layout(std140,set=3,binding=0) uniform Material {
    vec4 ambient;
    vec4 tint;
    vec4 surface; // normal strength, specular strength, shininess, light count
    vec4 orientation; // cosine, sine, flip X, flip Y
    vec4 flags; // has normal, has specular, linear output, receiver mask low
    vec4 extra; // receiver mask high
} m;
vec3 linear(vec3 c) {return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),step(vec3(.04045),c));}
vec3 srgb(vec3 c) {return mix(c*12.92,1.055*pow(c,vec3(1.0/2.4))-.055,step(vec3(.0031308),c));}
vec3 unit(vec3 v) {float d=length(v);return d>1e-8?v/d:vec3(0,0,1);}
bool inside_quad(vec2 p,vec4 ab,vec4 cd) {
    vec2 points[4]=vec2[4](ab.xy,ab.zw,cd.xy,cd.zw);bool inside=false;
    for(int i=0,j=3;i<4;j=i++) {
        vec2 a=points[i],b=points[j];
        if((a.y>p.y)!=(b.y>p.y) && p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)inside=!inside;
    }
    return inside;
}
float cookie(int offset,int width,int height,vec2 uv) {
    ivec2 at=clamp(ivec2(uv*vec2(width,height)),ivec2(0),ivec2(width-1,height-1));
    int index=at.y*width+at.x;return data[offset+index/4][index%4];
}
void main() {
    vec4 albedo=texture(Albedo,In.UV);
    vec3 base=linear(albedo.rgb)*m.tint.rgb;
    vec3 n=vec3(0,0,1);
    if(m.flags.x>.5)n=texture(Normal,In.UV).rgb*2-1;
    n.xy*=m.surface.x*m.orientation.zw;n=unit(n);
    n=unit(vec3(m.orientation.x*n.x-m.orientation.y*n.y,m.orientation.y*n.x+m.orientation.x*n.y,n.z));
    float spec=m.surface.y*(m.flags.y>.5?texture(Specular,In.UV).r:1);
    vec3 result=m.ambient.rgb*base;
    uint receiver=uint(m.flags.w)|(uint(m.extra.x)<<16);
    for(int i=0;i<int(m.surface.w);++i) {
        int at=i*6;vec4 p=data[at],c=data[at+1],direction=data[at+2],shadow=data[at+3],shape=data[at+4],other=data[at+5];
        uint mask=uint(shape.w)|(uint(other.x)<<16);if((mask&receiver)==0u)continue;
        bool directional=direction.z>.5,shaped=shadow.w>.5;
        vec2 delta=p.xy-World.xy;float d2=dot(delta,delta)/(p.w*p.w);
        if(!directional&&!shaped&&d2>=1)continue;
        float attenuation=directional||shaped?1:(1-d2)*(1-d2);
        if(shaped) {
            float cs=cos(direction.w),sn=sin(direction.w);
            vec2 q=vec2(-cs*delta.x-sn*delta.y,sn*delta.x-cs*delta.y);
            if(any(greaterThanEqual(abs(q),vec2(p.w))))continue;
            attenuation=cookie(int(shape.x),int(shape.y),int(shape.z),q/(2*p.w)+.5);
        }
        vec3 l=directional?unit(vec3(direction.xy,p.z)):unit(vec3(delta,p.z));
        int count=int(shadow.y);float blocked=0;
        for(int s=0;s<count;++s) {
            vec4 header=data[int(shadow.z)+s];bool hit=false;
            for(int q=0;q<int(header.y);++q) {
                int start=int(header.x)+q*2;
                if(inside_quad(World.xy,data[start],data[start+1])){hit=true;break;}
            }
            if(hit)blocked+=1.0/float(count);
        }
        attenuation*=1-blocked*clamp(shadow.x,0,1);
        float diffuse=max(0,dot(n,l));
        float highlight=diffuse>0?spec*pow(max(0,dot(n,unit(l+vec3(0,0,1)))),m.surface.z):0;
        result+=attenuation*min(c.a,16)*c.rgb*(base*diffuse+highlight);
    }
    result=clamp(result,0,1);
    fColor=vec4(m.flags.z>.5?result:srgb(result),albedo.a*m.tint.a);
}
