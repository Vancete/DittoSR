// DittoSR's frame generation (fg.cpp): the constants its passes share, and the field's conventions.
// The motion field, at the render size: xy = the vector in uv, this frame's position - the previous frame's (as
// letsgo-recomp's flow_field.frag, from which these passes come), z = the distance (linear depth), w = 1 (the
// distance's ratio from the previous frame to this one: unknown here, the depth test takes it as 1).

cbuffer C : register(b0) {
    float2 size;       // the pass's output size
    float2 sizeRcp;
    float2 mvToUv;     // the game's motion vector -> the previous uv - this uv (FSR's scale / the vectors' resolution)
    float2 mvRes;      // the motion vectors' resolution (render, or display with display-resolution vectors)
    float2 jitCancel;  // added to the vector when the motion vectors carry the jitter (uv)
    float nearZ;       // the camera's near and far planes (far unused with infinite depth)
    float farZ;
    float depthInv;    // 1: depth 1 = near (reversed-Z)
    float depthInf;    // 1: an infinite far plane
    float t;           // the in-between image's time, 0 the previous frame, 1 this one
    float hasPrev;     // 1: the previous frame's field is bound (the depth test, t2 of the warp)
};

SamplerState linearClamp : register(s0);

bool inside(float2 uv) { return all(uv >= 0.0) && all(uv < 1.0); }
