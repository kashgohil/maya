#include <metal_stdlib>
using namespace metal;

struct Vertex {
    packed_float3 position;
    packed_float3 normal;
    float4 color;
    float2 uv;
};

struct Uniforms {
    float4x4 model_matrix;
    float4x4 view_projection_matrix;
    float4 light_dir_world;
    float4 ambient_rgb;
    float4 light_diffuse_rgb;
    float4 camera_position_world;
    float4 specular_rgb_shininess;
};

struct VertexOut {
    float4 position [[position]];
    float3 world_position;
    float3 world_normal;
    float4 color;
    float2 uv;
};

vertex VertexOut vertexMain(uint vertexID [[vertex_id]],
                           constant Vertex* vertices [[buffer(0)]],
                           constant Uniforms& uniforms [[buffer(1)]]) {
    VertexOut out;

    float4 pos = float4(vertices[vertexID].position, 1.0);
    float4 world_pos = uniforms.model_matrix * pos;
    out.world_position = world_pos.xyz;
    out.position = uniforms.view_projection_matrix * world_pos;

    float3x3 normal_matrix = float3x3(
        uniforms.model_matrix[0].xyz,
        uniforms.model_matrix[1].xyz,
        uniforms.model_matrix[2].xyz
    );
    float3 n = vertices[vertexID].normal;
    out.world_normal = normalize(normal_matrix * n);

    out.color = vertices[vertexID].color;
    out.uv = vertices[vertexID].uv;

    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                            constant Uniforms& uniforms [[buffer(1)]],
                            texture2d<float> colorTexture [[texture(0)]],
                            sampler textureSampler [[sampler(0)]]) {
    float4 texColor = colorTexture.sample(textureSampler, in.uv);

    float3 N = normalize(in.world_normal);
    float3 L = normalize(uniforms.light_dir_world.xyz);
    float ndotl = saturate(dot(N, L));

    float3 V = normalize(uniforms.camera_position_world.xyz - in.world_position);
    float3 H = normalize(L + V);
    float shininess = max(uniforms.specular_rgb_shininess.w, 1.0);
    float spec_mask = pow(saturate(dot(N, H)), shininess);

    float3 albedo = texColor.rgb * in.color.rgb;
    float3 ambient_term = uniforms.ambient_rgb.rgb * albedo;
    float3 diffuse_term = ndotl * uniforms.light_diffuse_rgb.rgb * albedo;
    float3 specular_term =
        spec_mask * uniforms.specular_rgb_shininess.rgb * uniforms.light_diffuse_rgb.rgb;
    float3 rgb = ambient_term + diffuse_term + specular_term;

    return float4(rgb, texColor.a * in.color.a);
}

fragment float4 fragmentUnlit(VertexOut in [[stage_in]]) {
    return in.color;
}
