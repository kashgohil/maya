#include <metal_stdlib>
using namespace metal;

// Layouts match include/maya/rhi/vertex.hpp and include/maya/renderer/shader_constants.hpp.
// float3 occupies 16 bytes in Metal, matching the padded C++ Vec3 members of Vertex.
struct Vertex {
    float3 position;
    float3 normal;
    float4 color;
    float2 uv;
};

struct DirectionalLight {
    float4 direction_to_light;
    float4 radiance;
};

struct ViewConstants {
    float4x4 view_projection;
    float4 camera_position;
    float4 ambient;
    uint4 light_count;
    DirectionalLight lights[4];
};

struct DrawConstants {
    float4x4 model;
    float4 normal_matrix[3];
    float4 base_color;
    float4 material; // x metallic, y roughness
};

struct PresentConstants {
    float4 area; // left, bottom, right, top in normalized device coordinates
};

struct LitOut {
    float4 position [[position]];
    float3 world_position;
    float3 world_normal;
    float4 color;
};

vertex LitOut litVertex(uint id [[vertex_id]],
                        constant Vertex* vertices [[buffer(0)]],
                        constant DrawConstants& draw [[buffer(1)]],
                        constant ViewConstants& view [[buffer(2)]]) {
    const float4 world = draw.model * float4(vertices[id].position, 1.0);
    // Inverse transpose of the model's linear part keeps normals perpendicular under nonuniform scale.
    const float3x3 normal_matrix = float3x3(draw.normal_matrix[0].xyz, draw.normal_matrix[1].xyz,
                                            draw.normal_matrix[2].xyz);
    LitOut out;
    out.position = view.view_projection * world;
    out.world_position = world.xyz;
    out.world_normal = normal_matrix * vertices[id].normal;
    out.color = vertices[id].color;
    return out;
}

// Blinn-Phong direct lighting with material factors: metallic tints the highlight and removes the
// diffuse term; roughness widens the highlight.
fragment float4 litFragment(LitOut in [[stage_in]],
                            constant DrawConstants& draw [[buffer(1)]],
                            constant ViewConstants& view [[buffer(2)]]) {
    const float4 base = in.color * draw.base_color;
    const float metallic = saturate(draw.material.x);
    const float roughness = clamp(draw.material.y, 0.05, 1.0);
    const float roughness4 = roughness * roughness * roughness * roughness;
    const float shininess = clamp(2.0 / roughness4 - 2.0, 1.0, 2048.0);
    const float3 diffuse_color = base.rgb * (1.0 - metallic);
    const float3 specular_color = mix(float3(0.04), base.rgb, metallic);

    const float3 N = normalize(in.world_normal);
    const float3 V = normalize(view.camera_position.xyz - in.world_position);
    float3 rgb = view.ambient.rgb * base.rgb;
    const uint lights = min(view.light_count.x, 4u);
    for (uint i = 0; i < lights; ++i) {
        const float3 L = view.lights[i].direction_to_light.xyz;
        const float ndotl = saturate(dot(N, L));
        const float3 H = normalize(L + V);
        const float highlight = ndotl > 0.0 ? pow(saturate(dot(N, H)), shininess) : 0.0;
        rgb += view.lights[i].radiance.rgb * (diffuse_color * ndotl + specular_color * highlight);
    }
    return float4(rgb, base.a);
}

struct PresentOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 present_corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1),
                                      float2(1, 0), float2(1, 1), float2(0, 1)};

vertex PresentOut presentVertex(uint id [[vertex_id]], constant PresentConstants& present [[buffer(1)]]) {
    const float2 corner = present_corners[id % 6];
    PresentOut out;
    out.position = float4(mix(present.area.xy, present.area.zw, corner), 0.0, 1.0);
    out.uv = float2(corner.x, 1.0 - corner.y); // texture rows run top to bottom
    return out;
}

fragment float4 presentFragment(PresentOut in [[stage_in]], texture2d<float> image [[texture(0)]],
                                sampler filter [[sampler(0)]]) {
    return image.sample(filter, in.uv);
}
