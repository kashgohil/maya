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

// Debug lines and outlines (docs/renderer.md#debug-lines). Buffer 0 holds either lines (from, to,
// color) or outlines (world matrix columns, size, color); outlines are drawn from unit templates
// generated here, one instance per outline. Every segment becomes a screen-space quad, `width`
// pixels wide plus a pixel of soft edge.
struct DebugConstants {
    float4 viewport; // width, height, line width in pixels, opacity
    uint4 kind; // x: 0 lines, 1 box, 2 sphere, 3 capsule; y: circle segments (8, 16, or 32)
};

struct DebugOut {
    float4 position [[position]];
    float4 color;
    float across [[center_no_perspective]]; // pixels from the segment's centre line
    float half_width [[flat]];
};

constant uint debug_box_edges[24] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 1, 3, 4, 6, 5, 7, 0, 4, 1, 5, 2, 6, 3, 7};

// A unit circle point in one of three planes: 0 XY, 1 YZ, 2 ZX.
float3 debug_circle(uint plane, float angle) {
    const float c = cos(angle), s = sin(angle);
    return plane == 0 ? float3(c, s, 0) : plane == 1 ? float3(0, c, s) : float3(s, 0, c);
}

// Endpoint `end` of template line `line` with `n` segments per circle: xyz on the unit shape, w which
// cap it follows (capsules). Line counts match debug_template_lines in include/maya/world/debug_draw.hpp.
float4 debug_template(uint kind, uint line, uint end, uint n) {
    const float step = 2.0 * M_PI_F / float(n);
    if (kind == 1) {
        const uint corner = debug_box_edges[line * 2 + end];
        return float4(corner & 1 ? 1 : -1, corner & 2 ? 1 : -1, corner & 4 ? 1 : -1, 0);
    }
    if (kind == 2) return float4(debug_circle(line / n, float(line % n + end) * step), 0);
    // Capsule: rings around each cap's base, four sides, then arcs over the caps in the XY and ZY planes.
    if (line < 2 * n) {
        const float3 p = debug_circle(2, float(line % n + end) * step); // in the ZX plane
        return float4(p, line < n ? 1 : -1);
    }
    if (line < 2 * n + 4) {
        const uint side = line - 2 * n;
        const float3 p = side == 0 ? float3(1, 0, 0) : side == 1 ? float3(-1, 0, 0) : side == 2 ? float3(0, 0, 1) : float3(0, 0, -1);
        return float4(p, end == 0 ? -1 : 1);
    }
    const uint arc = line - (2 * n + 4); // 0 to 2n: n segments in XY, then n in ZY
    const uint k = arc % n; // the first half turn is the top cap, the second the bottom
    const float angle = float(k + end) * step;
    const float3 p = arc < n ? float3(cos(angle), sin(angle), 0) : float3(0, sin(angle), cos(angle));
    return float4(p, k < n / 2 ? 1 : -1);
}

vertex DebugOut debugVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                            const device float4* data [[buffer(0)]],
                            constant DebugConstants& debug [[buffer(1)]],
                            constant ViewConstants& view [[buffer(2)]]) {
    const uint line = vid / 6, corner = vid % 6;
    const uint kind = debug.kind.x;
    float3 a, b;
    float4 color;
    if (kind == 0) {
        const device float4* l = data + 3 * line;
        a = l[0].xyz;
        b = l[1].xyz;
        color = l[2];
    } else {
        const device float4* s = data + 6 * iid;
        const float4x4 world = float4x4(s[0], s[1], s[2], s[3]);
        const float3 size = s[4].xyz;
        color = s[5];
        const float4 ta = debug_template(kind, line, 0, debug.kind.y), tb = debug_template(kind, line, 1, debug.kind.y);
        // Boxes stretch by their half extents; spheres and capsules by their radius, and a capsule's
        // caps move apart by its half height.
        const float3 scale = kind == 1 ? size : float3(size.x);
        const float3 la = ta.xyz * scale + float3(0, ta.w * size.y, 0) * (kind == 3 ? 1.0 : 0.0);
        const float3 lb = tb.xyz * scale + float3(0, tb.w * size.y, 0) * (kind == 3 ? 1.0 : 0.0);
        a = (world * float4(la, 1)).xyz;
        b = (world * float4(lb, 1)).xyz;
    }
    // A thousandth of the distance toward the camera, so outlines on surfaces win the depth test.
    const float3 eye = view.camera_position.xyz;
    a = eye + (a - eye) * 0.999;
    b = eye + (b - eye) * 0.999;
    float4 ca = view.view_projection * float4(a, 1), cb = view.view_projection * float4(b, 1);
    DebugOut out;
    out.color = color;
    out.half_width = debug.viewport.z * 0.5;
    out.across = 0;
    const float near_w = 1e-3;
    if (ca.w < near_w && cb.w < near_w) { // behind the camera
        out.position = float4(0, 0, 2, 1);
        return out;
    }
    if (ca.w < near_w) ca = mix(ca, cb, (near_w - ca.w) / (cb.w - ca.w));
    if (cb.w < near_w) cb = mix(cb, ca, (near_w - cb.w) / (ca.w - cb.w));
    const float2 half_size = debug.viewport.xy * 0.5;
    const float2 sa = ca.xy / ca.w * half_size, sb = cb.xy / cb.w * half_size;
    const float length_pixels = length(sb - sa);
    const float2 d = length_pixels > 1e-4 ? (sb - sa) / length_pixels : float2(1, 0);
    const float2 n = float2(-d.y, d.x);
    // Two triangles: (a, -), (b, -), (a, +), (b, -), (b, +), (a, +).
    const bool at_b = corner == 1 || corner == 3 || corner == 4;
    const float side = corner == 0 || corner == 1 || corner == 3 ? -1.0 : 1.0;
    const float reach = out.half_width + 1.0; // a pixel of soft edge
    float4 c = at_b ? cb : ca;
    const float2 offset = n * (side * reach) + d * ((at_b ? 1.0 : -1.0) * reach);
    c.xy += offset / half_size * c.w;
    out.position = c;
    out.across = side * reach;
    return out;
}

fragment float4 debugFragment(DebugOut in [[stage_in]], constant DebugConstants& debug [[buffer(1)]]) {
    const float coverage = saturate(in.half_width + 0.5 - abs(in.across));
    return float4(in.color.rgb, in.color.a * coverage * debug.viewport.w);
}
