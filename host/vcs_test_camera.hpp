#pragma once
#include <array>
#include <cmath>
#include <cstdio>

namespace vcs::detail {
// Diagnostic replay only. The game's RenderWare convention is right/forward/up.
struct TestCameraBasis {
    std::array<float,3> right{ -1,0,0 }, forward{ 0,-1,0 }, up{ 0,0,1 };
};
inline TestCameraBasis test_camera_basis(const char *text) {
    float x=0,y=-1,z=0;
    if(!text || std::sscanf(text,"%f,%f,%f",&x,&y,&z)<2) return {};
    const float length=std::hypot(x,y,z);
    if(!std::isfinite(length) || length<.001f) return {};
    x/=length;y/=length;z/=length;
    const float horizontal=std::hypot(x,y);
    TestCameraBasis result;
    result.forward={x,y,z};
    if(horizontal>.00001f) result.right={y/horizontal,-x/horizontal,0};
    else result.right={1,0,0};
    const auto &r=result.right;
    result.up={r[1]*z-r[2]*y,r[2]*x-r[0]*z,r[0]*y-r[1]*x};
    return result;
}
} // namespace vcs::detail
