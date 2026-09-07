// PSP records are snapshotted into an SSBO. Word access works without optional
// 8/16-bit storage features, including the unaligned 3-byte s8 normal layouts.
layout(set=2,binding=0,std430) readonly buffer RawVertices { uint words[]; } raw;
struct ModelLight { vec4 vector,attenuation,spot,ambient,diffuse,specular; };
layout(set=2,binding=1,std140) uniform Model {
    uvec4 format,offsets,control,textureControl;
    vec4 world[3],textureMatrix[3],bones[24];
    vec4 base,ambient,diffuse,specular,emissive,globalAmbient,shadeS,shadeT;
    ModelLight lights[4];
} m;
uint byteAt(uint p) { return (raw.words[p>>2u]>>((p&3u)*8u))&255u; }
uint halfAt(uint p) { return byteAt(p)|(byteAt(p+1u)<<8u); }
uint wordAt(uint p) { return raw.words[p>>2u]; } // f32/color32 are aligned
uint scalarSize(uint t) { return t==3u ? 4u : t; }
float unsignedScalar(uint p,uint t) {
    if(t==1u) return float(byteAt(p))*(1.0/128.0);
    if(t==2u) return float(halfAt(p))*(1.0/32768.0);
    if(t==3u) return uintBitsToFloat(wordAt(p));
    return 0.0;
}
float signedScalar(uint p,uint t) {
    if(t==1u) return float(int(byteAt(p)<<24u)>>24)*(1.0/128.0);
    if(t==2u) return float(int(halfAt(p)<<16u)>>16)*(1.0/32768.0);
    return uintBitsToFloat(wordAt(p));
}
vec3 vectorAt(uint p,uint t) {
    uint s=scalarSize(t);
    return vec3(signedScalar(p,t),signedScalar(p+s,t),signedScalar(p+2u*s,t));
}
bool finiteFloat(float v) { return !isnan(v)&&!isinf(v); }
vec3 normalOr(vec3 n,vec3 fallback) {
    float l=dot(n,n);
    return finiteFloat(l)&&l>1e-30 ? n*(1.0/sqrt(l)) : fallback;
}
vec3 pointRows(vec4 a,vec4 b,vec4 c,vec3 p) {
    precise vec3 v=vec3(a.x,b.x,c.x)*p.x+vec3(a.y,b.y,c.y)*p.y+
                  vec3(a.z,b.z,c.z)*p.z+vec3(a.w,b.w,c.w);
    return v;
}
vec3 normalRows(vec4 a,vec4 b,vec4 c,vec3 p) {
    precise vec3 v=vec3(a.x,b.x,c.x)*p.x+vec3(a.y,b.y,c.y)*p.y+vec3(a.z,b.z,c.z)*p.z;
    return v;
}
float lightPow(float v,float e) { return e<=0.0 ? 1.0 : (v>0.0 ? pow(v,e) : v); }
vec4 vertexColor(uint p,uint t) {
    if(t<4u) return m.base;
    uint v=t==7u ? wordAt(p) : halfAt(p);
    if(t==7u) return vec4((uvec4(v)>>uvec4(0,8,16,24))&255u)/255.0;
    if(t==6u) return vec4((uvec4(v)>>uvec4(0,4,8,12))&15u)/15.0;
    uvec3 rgb=(uvec3(v)>>uvec3(0,5,t==4u ? 11 : 10))&uvec3(31,t==4u ? 63 : 31,31);
    rgb=(rgb<<uvec3(3,t==4u ? 2 : 3,3))|(rgb>>uvec3(2,t==4u ? 4 : 2,2));
    return vec4(vec3(rgb)/255.0,t==4u ? 1.0 : float(v>>15u));
}
vec4 lightVertex(vec4 color,vec3 position,vec3 normal) {
    vec4 amb=(m.control.y&1u)!=0u ? color : m.ambient;
    vec3 diff=(m.control.y&2u)!=0u ? color.rgb : m.diffuse.rgb;
    vec3 spec=(m.control.y&4u)!=0u ? color.rgb : m.specular.rgb;
    precise vec4 result=vec4(m.emissive.rgb+amb.rgb*m.globalAmbient.rgb,amb.a*m.globalAmbient.a);
    normal=normalOr(normal,vec3(0,0,1));
    for(uint i=0u;i<4u;++i) {
        ModelLight l=m.lights[i];
        if(l.ambient.w==0.0) continue;
        vec3 v=l.vector.xyz;
        float attenuation=1.0;
        if(l.vector.w!=0.0) {
            v-=position;
            float dd=dot(v,v);
            float distance=finiteFloat(dd)&&dd>0.0 ? sqrt(dd) : 0.0;
            v=normalOr(v,vec3(0,0,1));
            float denom=l.attenuation.x+l.attenuation.y*distance+l.attenuation.z*distance*distance;
            attenuation=clamp(denom>0.0 ? 1.0/denom : 0.0,0.0,1.0);
        }
        if(l.vector.w>=2.0) {
            float spot=dot(l.spot.xyz,v);
            if(!finiteFloat(spot)) spot=0.0;
            attenuation*=spot>=l.spot.w ? max(0.0,lightPow(spot,l.diffuse.w)) : 0.0;
        }
        result.rgb+=l.ambient.rgb*amb.rgb*attenuation;
        float factor=dot(v,normal);
        if(l.attenuation.w==2.0) factor=lightPow(factor,m.specular.w);
        if(factor>0.0) result.rgb+=l.diffuse.rgb*diff*(attenuation*factor);
        if(l.attenuation.w==1.0&&factor>=0.0) {
            float s=lightPow(dot(normalOr(v+vec3(0,0,1),vec3(0,0,1)),normal),m.specular.w);
            if(s>0.0) result.rgb+=l.specular.rgb*spec*(attenuation*s);
        }
    }
    // CPU from_float_color uses lround, before interpolation.
    return floor(clamp(result,0.0,1.0)*255.0+0.5)/255.0;
}
void decodeModel(out vec4 position,out vec4 color,out vec2 uv,out float q) {
    uint p=m.format.z+uint(gl_VertexIndex)*m.format.y;
    uint type=m.format.x,tc=type&3u,n=(type>>5u)&3u,w=(type>>9u)&3u;
    uv=vec2(unsignedScalar(p+m.offsets.x,tc),unsignedScalar(p+m.offsets.x+scalarSize(tc),tc));
    color=vertexColor(p+m.offsets.y,(type>>2u)&7u);
    vec3 pos=vectorAt(p+m.offsets.w,(type>>7u)&3u);
    vec3 normal=n==0u ? vec3(0,0,1) : vectorAt(p+m.offsets.z,n);
    if(w!=0u) {
        precise vec4 a=vec4(0),b=vec4(0),c=vec4(0);
        for(uint i=0u;i<m.format.w;++i) {
            float weight=unsignedScalar(p+i*scalarSize(w),w);
            if(weight==0.0) continue;
            a+=m.bones[i*3u]*weight;b+=m.bones[i*3u+1u]*weight;c+=m.bones[i*3u+2u]*weight;
        }
        pos=pointRows(a,b,c,pos);
        normal=normalRows(a,b,c,normal);
    }
    q=1.0;
    vec3 worldNormal=normalRows(m.world[0],m.world[1],m.world[2],normal);
    if(m.control.z!=0u) worldNormal=-worldNormal;
    worldNormal=normalOr(worldNormal,vec3(0,0,1));
    if(m.control.w==1u) {
        vec3 source=pos;
        if(m.textureControl.x==1u) source=vec3(uv,0);
        if(m.textureControl.x==2u) source=normalOr(normal,vec3(0));
        if(m.textureControl.x==3u) source=normal;
        vec3 stq=pointRows(m.textureMatrix[0],m.textureMatrix[1],m.textureMatrix[2],source);
        uv=stq.xy;q=stq.z;
    } else if(m.control.w==2u) {
        uv=(vec2(dot(m.shadeS.xyz,worldNormal),dot(m.shadeT.xyz,worldNormal))+1.0)*0.5;
    }
    if(m.control.x!=0u) {
        pos=pointRows(m.world[0],m.world[1],m.world[2],pos);
        color=lightVertex(color,pos,worldNormal);
    }
    position=vec4(pos,1);
}
