// Runs every function in shaders/SOGDecode.metal over all splats; run_metal_test.swift compares
// the output with the float64 Python reference (export_test_data.py).
#include "SOGDecode.metal"

kernel void sog_golden_test(device const SOGSplat*         splats   [[buffer(0)]],
                            constant SOGAssetConstants&    k        [[buffer(1)]],
                            constant float*                scaleLUT [[buffer(2)]],
                            constant float*                dcLUT    [[buffer(3)]],
                            device const half4*            palette  [[buffer(4)]],
                            constant float4&               dir      [[buffer(5)]],
                            device float*                  out      [[buffer(6)]],
                            uint                           i        [[thread_position_in_grid]])
{
    if (i >= k.count) {
        return;
    }
    const SOGSplat s = splats[i];
    const float3 p = sog_mean(s, k);
    const float4 q = sog_quat_wxyz(s.quat);
    const float3 sc = sog_scale(s.scaleOpacity, scaleLUT);
    const float3 dc = sog_color(s, dir.xyz, dcLUT, palette, k.shCoeffs, 0);
    const float3 col = sog_color(s, dir.xyz, dcLUT, palette, k.shCoeffs, 15);
    const float3x3 cov = sog_covariance_local(q, sc, k);

    device float* o = out + i * 24;
    o[0] = p.x;  o[1] = p.y;  o[2] = p.z;
    o[3] = q.x;  o[4] = q.y;  o[5] = q.z;  o[6] = q.w;
    o[7] = sc.x; o[8] = sc.y; o[9] = sc.z;
    o[10] = sog_opacity(s.scaleOpacity);
    o[11] = dc.x; o[12] = dc.y; o[13] = dc.z;
    o[14] = col.x; o[15] = col.y; o[16] = col.z;
    o[17] = float(sog_label(s));
    o[18] = cov[0][0]; o[19] = cov[1][0]; o[20] = cov[2][0];
    o[21] = cov[1][1]; o[22] = cov[2][1]; o[23] = cov[2][2];
}
