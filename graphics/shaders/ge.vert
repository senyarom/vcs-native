#version 450
#ifdef PACKED_0115
layout(location=0) in uvec2 packedUv;
layout(location=1) in uint packedColor;
layout(location=2) in ivec2 packedXY;
layout(location=3) in int packedZ;
#elif defined(RAW_MODEL)
#extension GL_GOOGLE_include_directive : require
#include "ge_model.glsl"
#else
layout(location=0) in vec4 position;
layout(location=1) in vec4 color;
layout(location=2) in vec2 uv;
layout(location=3) in float q;
layout(location=4) in float fog;
#endif
layout(set=1,binding=0,std140) uniform Draw {
    vec4 row0,row1,row2,row3,viewZ,uvTransform,fogTransform;
    uvec4 mode;
    vec4 colorMul,colorAdd;
    uvec4 pixel,format;
} d;
layout(location=0) out vec4 outColor;
layout(location=1) out vec2 outUv;
layout(location=2) out float outQ;
layout(location=3) out float outFog;
void main() {
#ifdef PACKED_0115
    vec4 position=vec4(vec3(packedXY,packedZ)*(1.0/32768.0),1.0);
    uvec3 rgb=(uvec3(packedColor)>>uvec3(0,5,10))&31u;
    // Match the CPU's five-bit replication, including its 8-bit rounding.
    vec4 color=vec4(vec3((rgb<<3u)|(rgb>>2u))/255.0,float(packedColor>>15u));
    vec2 uv=vec2(packedUv)*(1.0/128.0);
    float q=1.0;
    float fog=1.0;
#elif defined(RAW_MODEL)
    vec4 position,color;vec2 uv;float q;
    decodeModel(position,color,uv,q);
    float fog=1.0;
#endif
    float w=abs(position.w)<1e-12 ? 1.0 : position.w;
    if(d.mode.x==1u) {
        w=dot(d.row3,position);
        if(abs(w)<1e-12) w=1.0;
        float z=dot(d.row2,position);
        if(d.mode.y==0u) z=clamp(z,0.0,max(w,0.0));
        gl_Position=vec4(dot(d.row0,position),dot(d.row1,position),z,w);
        outUv=uv*d.uvTransform.xy+d.uvTransform.zw;
        outFog=clamp((dot(d.viewZ,position)+d.fogTransform.x)*d.fogTransform.y,0.0,1.0);
    } else {
        gl_Position=vec4((position.xy*d.uvTransform.xy-1.0)*w,
                         clamp(position.z/65535.0,0.0,1.0)*w,w);
        outUv=uv;
        outFog=fog;
    }
    outColor=color;
    if(d.mode.z!=0u) outColor=floor(clamp(color*d.colorMul+d.colorAdd,0.0,1.0)*255.0)/255.0;
    outQ=q;
}
