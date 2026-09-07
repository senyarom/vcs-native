#version 450
layout(set=0,binding=0) uniform sampler2D sourceTexture;
layout(set=1,binding=0,std140) uniform Draw {
    vec4 row0,row1,row2,row3,viewZ,uvTransform,fogTransform;
    uvec4 mode;
    vec4 colorMul,colorAdd;
    uvec4 pixel,format;
} d;
layout(location=0) in vec4 color;
layout(location=1) in vec2 uv;
layout(location=2) in float q;
layout(location=3) in float fog;
layout(location=0) out vec4 outputColor;
uvec4 bytes(uint v) { return uvec4(v,v>>8,v>>16,v>>24)&255u; }
bool alphaPass(uint fn,uint a,uint b) {
    switch(fn&7u) {
    case 0u:return false; case 1u:return true; case 2u:return a==b;
    case 3u:return a!=b; case 4u:return a<b; case 5u:return a<=b;
    case 6u:return a>b; case 7u:return a>=b;
    }
    return true;
}
vec4 quantize(vec4 c,uint format) {
    c=clamp(c,0.0,1.0);
    if(format==0u) { c.rgb=floor(c.rgb*vec3(31,63,31)+0.5)/vec3(31,63,31); c.a=1.0; }
    else if(format==1u) { c.rgb=floor(c.rgb*31.0+0.5)/31.0; c.a=step(0.5,c.a); }
    else if(format==2u) c=floor(c*15.0+0.5)/15.0;
    return c;
}
void main() {
    vec4 c=clamp(color,0.0,1.0);
    uvec4 t=bytes(d.pixel.y), a=bytes(d.pixel.x), f=bytes(d.pixel.w);
    if(t.w!=0u) {
        vec4 texel=texture(sourceTexture,uv/(abs(q)<1e-20 ? 1.0:q),d.fogTransform.z);
        vec3 env=vec3(bytes(d.pixel.z).xyz)/255.0;
        if(t.x==0u) { c.rgb*=texel.rgb; if(t.y!=0u)c.a*=texel.a; }
        else if(t.x==1u)c.rgb=mix(c.rgb,texel.rgb,t.y!=0u ? texel.a:1.0);
        else if(t.x==2u) { c.rgb=mix(c.rgb,env,texel.rgb); if(t.y!=0u)c.a*=texel.a; }
        else if(t.x==3u) { c.rgb=texel.rgb; if(t.y!=0u)c.a=texel.a; }
        else if(t.x==4u) { c.rgb=min(c.rgb+texel.rgb,1.0); if(t.y!=0u)c.a*=texel.a; }
        if(t.z!=0u)c.rgb=min(c.rgb*2.0,1.0);
    }
    if(f.w!=0u)c.rgb=mix(vec3(f.xyz)/255.0,c.rgb,clamp(fog,0.0,1.0));
    if(a.x!=0u && !alphaPass(a.y,uint(floor(clamp(c.a,0.0,1.0)*255.0+0.5))&a.w,a.z&a.w)) discard;
    outputColor=quantize(c,d.format.x&3u);
}
