#version 300 es
precision highp float;
layout(location=0) in vec4 aLinear;
layout(location=1) in vec4 aPlace;
layout(location=2) in vec4 aShape;
layout(location=3) in vec4 aFill;
layout(location=4) in vec4 aStroke;
layout(location=5) in vec2 aCorner;
uniform vec2 uOutput;
out vec2 vLocal;
flat out vec4 vSize;
flat out vec4 vFill;
flat out vec4 vStroke;
flat out float vKind;
void main() {
    vLocal = aCorner * (aPlace.zw + vec2(aShape.y + aShape.z));
    vec2 pos = vec2(aLinear.x * vLocal.x + aLinear.z * vLocal.y + aPlace.x,
                    aLinear.y * vLocal.x + aLinear.w * vLocal.y + aPlace.y);
    gl_Position = vec4(pos / uOutput * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
    vSize = vec4(aPlace.zw, aShape.x, aShape.y);
    vFill = aFill;
    vStroke = aStroke;
    vKind = aShape.w;
}
