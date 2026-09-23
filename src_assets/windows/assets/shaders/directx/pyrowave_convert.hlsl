// PyroWave stores normalized floating-point Y/Cb/Cr. R16 targets increase
// input precision; they are not P010 and do not shift ten-bit integer codes.
Texture2D<float4> source_texture : register(t0);

cbuffer rotation_parameters : register(b1) {
    int rotation_steps;
};

cbuffer profile_parameters : register(b2) {
    uint output_pq;
    uint output_primaries_2020;
    uint output_matrix_2020;
    uint output_full_range;
    uint output_444;
    uint output_chroma_left;
    uint input_linear;
    uint input_hdr;
};

struct vertex_t {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

vertex_t main_vs(uint vertex_id : SV_VertexID) {
    vertex_t output;
    float2 uv;
    if (vertex_id == 0) {
        output.position = float4(-1, -1, 0, 1);
        uv = float2(0, 1);
    } else if (vertex_id == 1) {
        output.position = float4(-1, 3, 0, 1);
        uv = float2(0, -1);
    } else {
        output.position = float4(3, -1, 0, 1);
        uv = float2(2, 1);
    }
    // Integer quarter turns avoid trig rounding at rotated image boundaries.
    uint turn = uint(rotation_steps + 4) & 3;
    if (turn == 1) uv = float2(1 - uv.y, uv.x);
    else if (turn == 2) uv = 1 - uv;
    else if (turn == 3) uv = float2(uv.y, 1 - uv.x);
    output.uv = uv;
    return output;
}

float srgb_to_linear(float value) {
    value = saturate(value);
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

float3 load_linear(int2 position, int2 dimensions) {
    float3 rgb = source_texture.Load(int3(clamp(position, int2(0, 0), dimensions - 1), 0)).rgb;
    if (input_linear != 0) return rgb; // Windows scRGB: linear BT.709, 1.0 = 80 nit.
    return float3(srgb_to_linear(rgb.r), srgb_to_linear(rgb.g), srgb_to_linear(rgb.b));
}

float3 sample_linear(float2 uv) {
    uint width, height;
    source_texture.GetDimensions(width, height);
    int2 dimensions = int2(width, height);
    float2 position = uv * float2(dimensions) - 0.5;
    int2 origin = int2(floor(position));
    float2 fraction = frac(position);
    // Filter in linear light, including SDR UNORM sources whose existing SRV
    // is not an sRGB view. All taps clamp to the captured image boundary.
    float3 top = lerp(load_linear(origin, dimensions), load_linear(origin + int2(1, 0), dimensions), fraction.x);
    float3 bottom = lerp(load_linear(origin + int2(0, 1), dimensions), load_linear(origin + int2(1, 1), dimensions), fraction.x);
    return lerp(top, bottom, fraction.y);
}

float bt709_oetf(float value) {
    value = saturate(value);
    return value < 0.018 ? 4.5 * value : 1.099 * pow(value, 0.45) - 0.099;
}

float pq_oetf(float sc_rgb_value) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float powered = pow(saturate(sc_rgb_value * (80.0 / 10000.0)), m1);
    return pow((c1 + c2 * powered) / (1 + c3 * powered), m2);
}

float3 encoded_rgb(float2 uv) {
    float3 rgb = sample_linear(uv);
    if (input_linear != 0 && input_hdr != 0 && output_pq == 0) {
        // Defined HDR-to-SDR shoulder, not a creative/scene-adaptive tone map.
        // Preserve luminance <= 0.75; continuously compress highlights to 1.
        // Never apply this to an SDR desktop captured as scRGB FP16.
        float luminance = max(dot(rgb, float3(0.2126, 0.7152, 0.0722)), 0);
        if (luminance > 0.75) {
            float excess = luminance - 0.75;
            float mapped = 0.75 + excess / (1 + excess / 0.25);
            rgb *= mapped / luminance;
        }
    }
    if (output_primaries_2020 != 0) {
        rgb = float3(
            dot(rgb, float3(0.627403896, 0.329283038, 0.043313066)),
            dot(rgb, float3(0.069097289, 0.919540395, 0.011362316)),
            dot(rgb, float3(0.016391439, 0.088013308, 0.895595253)));
    }
    // Gamut conversion precedes the independent transfer/matrix choices.
    // A single return also avoids FXC's false uninitialized-return warning when
    // it inlines this function several times inside the chroma filter branch.
    float3 result = float3(bt709_oetf(rgb.r), bt709_oetf(rgb.g), bt709_oetf(rgb.b));
    if (output_pq != 0) {
        result = float3(pq_oetf(rgb.r), pq_oetf(rgb.g), pq_oetf(rgb.b));
    }
    return result;
}

float3 ycbcr(float2 uv) {
    float3 rgb = encoded_rgb(uv);
    float kr = output_matrix_2020 != 0 ? 0.2627 : 0.2126;
    float kb = output_matrix_2020 != 0 ? 0.0593 : 0.0722;
    float y = dot(rgb, float3(kr, 1 - kr - kb, kb));
    return float3(y, (rgb.b - y) / (2 * (1 - kb)), (rgb.r - y) / (2 * (1 - kr)));
}

float main_y_ps(vertex_t input) : SV_Target {
    float y = ycbcr(input.uv).x;
    return output_full_range != 0 ? saturate(y) : saturate((16.0 + 219.0 * y) / 255.0);
}

float2 main_uv_ps(vertex_t input) : SV_Target {
    float2 chroma;
    if (output_444 != 0) {
        // Siting has no spatial effect without subsampling.
        chroma = ycbcr(input.uv).yz;
    } else {
        // UV viewport pixels cover 2x2 luma pixels. Derivatives carry the
        // rotation and scaling, so left siting remains left in output space.
        float2 dx = ddx(input.uv) * 0.25;
        float2 dy = ddy(input.uv) * 0.25;
        if (output_chroma_left != 0) {
            // Separable [1,2,1]/4 horizontal, [1,1]/2 vertical filter,
            // centered on the left luma column (half a luma pixel to the left).
            chroma = (ycbcr(input.uv - 3 * dx - dy).yz +
                      2 * ycbcr(input.uv - dx - dy).yz +
                      ycbcr(input.uv + dx - dy).yz +
                      ycbcr(input.uv - 3 * dx + dy).yz +
                      2 * ycbcr(input.uv - dx + dy).yz +
                      ycbcr(input.uv + dx + dy).yz) / 8;
        } else {
            chroma = (ycbcr(input.uv - dx - dy).yz + ycbcr(input.uv + dx - dy).yz +
                      ycbcr(input.uv - dx + dy).yz + ycbcr(input.uv + dx + dy).yz) / 4;
        }
    }
    // The codec's floating-point VUI convention is independent of UNORM
    // storage precision. In particular neutral chroma is 128/255, not 0.5.
    float scale = output_full_range != 0 ? 1.0 : 224.0 / 255.0;
    return saturate(chroma * scale + 128.0 / 255.0);
}
