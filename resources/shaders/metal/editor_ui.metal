#include <metal_stdlib>
using namespace metal;

// Dear ImGui vertices (ImDrawVert): position and uv in points/texels, color as packed RGBA8.
struct UiVertex {
    packed_float2 position;
    packed_float2 uv;
    uint color;
};

struct UiConstants {
    float2 scale; // points to clip space
    float2 translate;
};

struct UiOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
};

vertex UiOut uiVertex(uint id [[vertex_id]],
                      constant UiVertex* vertices [[buffer(0)]],
                      constant UiConstants& ui [[buffer(1)]]) {
    const UiVertex v = vertices[id];
    UiOut out;
    out.position = float4(float2(v.position) * ui.scale + ui.translate, 0.0, 1.0);
    out.uv = float2(v.uv);
    out.color = unpack_unorm4x8_to_float(v.color); // byte 0 is red
    return out;
}

// Straight alpha, blended by the pipeline. Font texels are white with coverage in alpha.
fragment float4 uiFragment(UiOut in [[stage_in]], texture2d<float> image [[texture(0)]],
                           sampler filter [[sampler(0)]]) {
    return in.color * image.sample(filter, in.uv);
}
