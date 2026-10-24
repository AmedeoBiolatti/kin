uniform highp sampler2DArray uShadow;
uniform bool uShadowPass, uReceivesShadow;
uniform int uShadowCount;
uniform mat4 uShadowMatrix[4];
uniform vec4 uShadowEnds, uShadowBlend, uShadowBias, uShadowOffset;
uniform float uShadowTexel, uShadowStrength;
float shadow_cascade(int layer,vec3 n) {
    vec4 clip=uShadowMatrix[layer]*vec4(vWorld+n*uShadowOffset[layer],1.0);
    vec3 p=clip.xyz/clip.w;
    vec2 uv=p.xy*0.5+0.5;
    if (p.z<0.0 || p.z>1.0 || any(lessThan(uv,vec2(0))) || any(greaterThan(uv,vec2(1)))) return 1.0;
    float seen=0.0;
    for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) {
        float depth=texture(uShadow,vec3(uv+vec2(float(x),float(y))*uShadowTexel,float(layer))).r;
        seen += p.z+uShadowBias[layer]>=depth ? 1.0 : 0.0;
    }
    return seen/9.0;
}
float sunlight() {
    if (uShadowCount==0 || !uReceivesShadow) return 1.0;
    float depth=dot(vWorld-uEye,uForward);
    if (depth>uShadowEnds[uShadowCount-1]) return 1.0;
    int layer=0;
    for (int i=0;i<3;++i) if (i<uShadowCount-1 && depth>uShadowEnds[i]) layer=i+1;
    vec3 n=dot(vNormal,vNormal)>1e-20 ? normalize(vNormal)*(gl_FrontFacing ? 1.0 : -1.0) : normalize(cross(dFdx(vWorld),dFdy(vWorld)));
    float seen=shadow_cascade(layer,n);
    if (layer<uShadowCount-1 && depth>uShadowBlend[layer]) {
        float w=smoothstep(uShadowBlend[layer],uShadowEnds[layer],depth);
        seen=mix(seen,shadow_cascade(layer+1,n),w);
    }
    return mix(1.0,seen,uShadowStrength);
}
