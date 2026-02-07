#include <metal_stdlib>
using namespace metal;

struct Vertex {
    float3 position;
    float4 color;
};

struct Uniforms {
    float4x4 model_matrix;
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VertexOut vertexMain(uint vertexID [[vertex_id]], 
                           constant Vertex* vertices [[buffer(0)]],
                           constant Uniforms& uniforms [[buffer(1)]]) {
    VertexOut out;
    
    float4 pos = float4(vertices[vertexID].position, 1.0);
    
    // Apply the transformation matrix
    out.position = uniforms.model_matrix * pos;
    out.color = vertices[vertexID].color;
    
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
    return in.color;
}
