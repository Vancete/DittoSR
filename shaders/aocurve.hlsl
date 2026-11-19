// Ditto Relight (experimental), the curve's table (flags 262144, ao_curve_lut): 4096 entries over [0, 16] by the square
// root of the colour - entry k the curve at 16 x ((k / 4095)^2) (HDR's highlights in it too), as aocompose.hlsl applies it (linear HDR: ^ 1 / 2.2 in, ^ 2.2
// out). Built when the curve changes; the composition reads it with a bilinear tap a channel in place of the spline.
// u0 = the table (4096 x 1).
#include "ao_common.hlsli"

AOFMT("r32f") RWTexture2D<float> Lut : register(u0);

[numthreads(64, 1, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (id.x >= 4096u) return;
    float s = float(id.x) / 4095.0, o = 16.0 * s * s;
    bool lin = pivot < 0.2;
    float c = curveAt(lin ? pow(o, 1.0 / 2.2) : o);
    Lut[uint2(id.x, 0u)] = lin ? pow(c, 2.2) : c;
}
