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

// Texture thumbnails for the Assets panel (texture_thumbnails.cpp): one triangle covering the target,
// sampled with automatic mip selection, in the display's encoding.
struct ThumbnailOut {
    float4 position [[position]];
    float2 uv;
};

struct ThumbnailConstants {
    uint mode;   // 0: color (sampled as linear, shown sRGB-encoded over a checkerboard), 1: data, 2: normal
    float cell;  // checkerboard cell size in pixels
};

vertex ThumbnailOut thumbnailVertex(uint id [[vertex_id]]) {
    const float2 corner = float2(float((id << 1) & 2), float(id & 2)); // (0,0), (2,0), (0,2)
    ThumbnailOut out;
    out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(corner.x, 1.0 - corner.y);
    return out;
}

fragment float4 thumbnailFragment(ThumbnailOut in [[stage_in]], texture2d<float> image [[texture(0)]],
                                  sampler filter [[sampler(0)]], constant ThumbnailConstants& constants [[buffer(0)]]) {
    const float4 texel = image.sample(filter, in.uv);
    if (constants.mode == 2) { // tangent-space x in red, y in alpha; z rebuilt
        const float2 xy = float2(texel.r, texel.a) * 2.0 - 1.0;
        const float3 normal = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
        return float4(normal * 0.5 + 0.5, 1.0);
    }
    if (constants.mode == 1) return float4(texel.rgb, 1.0); // stored values as they are
    const float3 linear = saturate(texel.rgb);
    const float3 encoded = select(1.055 * pow(linear, 1.0 / 2.4) - 0.055, 12.92 * linear, linear <= 0.0031308);
    const float2 cell = floor(in.position.xy / constants.cell);
    const float checker = fmod(cell.x + cell.y, 2.0) < 1.0 ? 0.32 : 0.22;
    return float4(mix(float3(checker), encoded, texel.a), 1.0);
}
