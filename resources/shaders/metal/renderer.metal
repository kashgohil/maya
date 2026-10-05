#include <metal_stdlib>
using namespace metal;

// Layouts match include/maya/rhi/vertex.hpp and include/maya/renderer/shader_constants.hpp.
// float3 occupies 16 bytes in Metal, matching the padded C++ Vec3 members of Vertex.
struct Vertex {
    float3 position;
    float3 normal;
    float4 color;
    float2 uv;
    float4 tangent; // xyz along increasing u; w the bitangent's sign
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
    float4x4 inverse_view_projection; // for the sky's view rays
    float4 environment; // x intensity, y cos and z sin of the rotation, w the specular cube's last level
    uint4 environment_flags; // x an environment lights the scene, y the sky is drawn
    float4 irradiance[9]; // spherical-harmonic coefficients, rgb
};

struct DrawConstants { // per draw, for the vertex stage
    float4x4 model;
    float4 normal_matrix[3];
};

struct MaterialConstants { // per material, for the fragment stage
    float4 base_color;
    float4 factors; // x metallic, y roughness, z normal scale, w occlusion strength
    float4 emissive; // rgb emitted light, w alpha cutoff
    uint4 flags; // x a bit per texture slot in use, y alpha mode (0 opaque, 1 mask, 2 blend)
    float4 uv_transform; // rows of the texture-coordinate matrix: KHR_texture_transform's rotation and scale
    float4 uv_offset; // xy
};

struct PresentConstants {
    float4 area; // left, bottom, right, top in normalized device coordinates
};

struct LitOut {
    float4 position [[position]];
    float3 world_position;
    float3 world_normal;
    float4 world_tangent; // xyz, and w the bitangent's sign
    float4 color;
    float2 uv;
};

vertex LitOut litVertex(uint id [[vertex_id]],
                        constant Vertex* vertices [[buffer(0)]],
                        constant DrawConstants& draw [[buffer(1)]],
                        constant ViewConstants& view [[buffer(2)]]) {
    const Vertex v = vertices[id];
    const float4 world = draw.model * float4(v.position, 1.0);
    // Inverse transpose of the model's linear part keeps normals perpendicular under nonuniform scale;
    // tangents lie in the surface, so the model matrix itself carries them.
    const float3x3 normal_matrix = float3x3(draw.normal_matrix[0].xyz, draw.normal_matrix[1].xyz,
                                            draw.normal_matrix[2].xyz);
    const float3x3 linear = float3x3(draw.model[0].xyz, draw.model[1].xyz, draw.model[2].xyz);
    LitOut out;
    out.position = view.view_projection * world;
    out.world_position = world.xyz;
    out.world_normal = normal_matrix * v.normal;
    out.world_tangent = float4(linear * v.tangent.xyz, v.tangent.w);
    out.color = v.color;
    out.uv = v.uv;
    return out;
}

// glTF's metallic-roughness BRDF (docs/renderer.md#materials), as tests/support/shading.hpp mirrors it.
constant float min_roughness = 0.045; // keeps GGX finite for a mirror under a directional light

float3 fresnel_schlick(float3 f0, float VdotH) {
    return f0 + (1.0 - f0) * pow(1.0 - VdotH, 5.0);
}
// GGX normal distribution, alpha the squared roughness.
float ggx(float NdotH, float alpha) {
    const float a2 = alpha * alpha;
    const float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (M_PI_F * d * d);
}
// Height-correlated Smith visibility, the masking-shadowing term over 4 NdotL NdotV.
float smith_visibility(float NdotL, float NdotV, float alpha) {
    const float a2 = alpha * alpha;
    const float v = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    const float l = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return v + l > 0.0 ? 0.5 / (v + l) : 0.0;
}
// Environments (docs/renderer.md#environments). A world direction's direction in the environment, which
// is turned by the rotation about +Y.
float3 to_environment(constant ViewConstants& view, float3 d) {
    const float c = view.environment.y, s = view.environment.z;
    return float3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
}
// Irradiance from the spherical-harmonic coefficients (l <= 2), as cook_environment computes them.
float3 environment_irradiance(constant ViewConstants& view, float3 n) {
    const float basis[9] = {0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x, 1.092548 * n.x * n.y,
                            1.092548 * n.y * n.z, 0.315392 * (3.0 * n.z * n.z - 1.0), 1.092548 * n.x * n.z,
                            0.546274 * (n.x * n.x - n.y * n.y)};
    float3 e = float3(0.0);
    for (uint i = 0; i < 9; ++i) e += view.irradiance[i].rgb * basis[i];
    return max(e, float3(0.0));
}
// The split-sum table holds 0 and 1 at its first and last texels' centres (brdf_table_size texels a side).
constant float brdf_table_size = 64.0;
float2 table_uv(float2 value) { return (value * (brdf_table_size - 1.0) + 0.5) / brdf_table_size; }
// The equirectangular background's coordinates: u 0.5 along -Z, growing toward +X; v 0 straight up.
float2 equirect_uv(float3 d) {
    return float2(0.5 + atan2(d.x, -d.z) / (2.0 * M_PI_F), acos(clamp(d.y, -1.0, 1.0)) / M_PI_F);
}

// Texture slots, as MaterialSlot in include/maya/renderer/render_snapshot.hpp.
constant uint slot_base_color = 0, slot_metallic_roughness = 1, slot_normal = 2, slot_occlusion = 3, slot_emissive = 4;
bool has_map(constant MaterialConstants& material, uint slot) { return (material.flags.x >> slot & 1u) != 0; }

fragment float4 litFragment(LitOut in [[stage_in]], bool front [[front_facing]],
                            constant ViewConstants& view [[buffer(2)]],
                            constant MaterialConstants& material [[buffer(3)]],
                            texture2d<float> base_color_map [[texture(0)]], sampler base_color_sampler [[sampler(0)]],
                            texture2d<float> metallic_roughness_map [[texture(1)]], sampler metallic_roughness_sampler [[sampler(1)]],
                            texture2d<float> normal_map [[texture(2)]], sampler normal_sampler [[sampler(2)]],
                            texture2d<float> occlusion_map [[texture(3)]], sampler occlusion_sampler [[sampler(3)]],
                            texture2d<float> emissive_map [[texture(4)]], sampler emissive_sampler [[sampler(4)]],
                            texturecube<float> specular_cube [[texture(5)]], sampler environment_sampler [[sampler(5)]],
                            texture2d<float> brdf_table [[texture(7)]], sampler table_sampler [[sampler(7)]]) {
    const float2 uv = float2(dot(material.uv_transform.xy, in.uv), dot(material.uv_transform.zw, in.uv)) + material.uv_offset.xy;
    float4 base = in.color * material.base_color;
    if (has_map(material, slot_base_color)) base *= base_color_map.sample(base_color_sampler, uv);
    const uint alpha_mode = material.flags.y;
    if (alpha_mode == 1 && base.a < material.emissive.w) discard_fragment();

    float metallic = material.factors.x, roughness = material.factors.y;
    if (has_map(material, slot_metallic_roughness)) {
        const float4 sample = metallic_roughness_map.sample(metallic_roughness_sampler, uv);
        roughness *= sample.g;
        metallic *= sample.b;
    }
    metallic = saturate(metallic);
    roughness = clamp(roughness, min_roughness, 1.0);
    const float alpha = roughness * roughness;

    // The geometric frame; a double-sided surface seen from behind turns all of it around, as glTF says.
    float3 N = normalize(in.world_normal);
    if (has_map(material, slot_normal)) {
        const float3 T = normalize(in.world_tangent.xyz - N * dot(N, in.world_tangent.xyz));
        const float3 B = cross(N, T) * (in.world_tangent.w < 0.0 ? -1.0 : 1.0);
        // x in red, green, and blue and y in alpha (docs/assets.md#textures); z rebuilt, then x and y scaled.
        const float4 sample = normal_map.sample(normal_sampler, uv);
        const float2 xy = float2(sample.r, sample.a) * 2.0 - 1.0;
        const float3 tangent_normal = normalize(float3(xy * material.factors.z, sqrt(saturate(1.0 - dot(xy, xy)))));
        N = normalize(T * tangent_normal.x + B * tangent_normal.y + N * tangent_normal.z);
    }
    if (!front) N = -N;

    const float3 c_diff = base.rgb * (1.0 - metallic);
    const float3 f0 = mix(float3(0.04), base.rgb, metallic);
    const float3 V = normalize(view.camera_position.xyz - in.world_position);
    const float NdotV = max(dot(N, V), 1e-4);

    // Light from the surroundings: the environment's, or the uniform ambient light. The split-sum table
    // gives the specular's share; diffuse takes what it leaves. Occlusion darkens both.
    float occlusion = 1.0;
    if (has_map(material, slot_occlusion)) occlusion = 1.0 + material.factors.w * (occlusion_map.sample(occlusion_sampler, uv).r - 1.0);
    const float2 scale_bias = brdf_table.sample(table_sampler, table_uv(float2(NdotV, roughness))).rg;
    const float3 specular_share = f0 * scale_bias.x + scale_bias.y;
    float3 diffuse_light = view.ambient.rgb, specular_light = view.ambient.rgb;
    if (view.environment_flags.x != 0) {
        const float intensity = view.environment.x;
        diffuse_light = environment_irradiance(view, to_environment(view, N)) / M_PI_F * intensity;
        const float3 R = to_environment(view, reflect(-V, N));
        specular_light = specular_cube.sample(environment_sampler, R, level(roughness * view.environment.w)).rgb * intensity;
    }
    float3 rgb = (c_diff * (1.0 - specular_share) * diffuse_light + specular_share * specular_light) * occlusion;

    const uint lights = min(view.light_count.x, 4u);
    for (uint i = 0; i < lights; ++i) {
        const float3 L = view.lights[i].direction_to_light.xyz;
        const float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;
        const float3 H = normalize(L + V);
        const float NdotH = saturate(dot(N, H)), VdotH = saturate(dot(V, H));
        const float3 F = fresnel_schlick(f0, VdotH);
        const float3 diffuse = (1.0 - F) * c_diff / M_PI_F;
        const float3 specular = F * ggx(NdotH, alpha) * smith_visibility(NdotL, NdotV, alpha);
        rgb += view.lights[i].radiance.rgb * (diffuse + specular) * NdotL;
    }

    float3 emitted = material.emissive.rgb;
    if (has_map(material, slot_emissive)) emitted *= emissive_map.sample(emissive_sampler, uv).rgb;
    rgb += emitted;
    return float4(rgb, alpha_mode == 2 ? base.a : 1.0);
}

// The sky: one triangle over the view at the far plane, behind every surface, showing the environment's
// background along each pixel's view ray.
struct SkyOut {
    float4 position [[position]];
    float2 ndc;
};

vertex SkyOut skyVertex(uint id [[vertex_id]]) {
    const float2 corner = float2(float((id << 1) & 2), float(id & 2)) * 2.0 - 1.0; // (-1,-1), (3,-1), (-1,3)
    SkyOut out;
    out.position = float4(corner, 1.0, 1.0);
    out.ndc = corner;
    return out;
}

fragment float4 skyFragment(SkyOut in [[stage_in]], constant ViewConstants& view [[buffer(2)]],
                            texture2d<float> background [[texture(6)]], sampler environment_sampler [[sampler(5)]]) {
    const float4 far = view.inverse_view_projection * float4(in.ndc, 1.0, 1.0);
    const float3 ray = normalize(far.xyz / far.w - view.camera_position.xyz);
    // The finest level: the seam where u wraps would otherwise pick a coarse one.
    const float3 sky = background.sample(environment_sampler, equirect_uv(to_environment(view, ray)), level(0.0)).rgb;
    return float4(sky * view.environment.x, 1.0);
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

// Exposure and tone mapping (docs/renderer.md#exposure-and-tone-mapping): the scene's HDR light,
// scaled by the camera's exposure, mapped to the display by AgX or Khronos PBR Neutral, and written
// sRGB-encoded to the view's RGBA8 target. One triangle covers the target; each pixel reads its own
// scene texel.
struct ToneMapConstants {
    float exposure; // 1 / (1.2 x 2^EV100)
    uint tone_mapping; // 0 AgX, 1 PBR Neutral
    uint view; // 0 the image, 1 luminance, 2 false-color exposure
    uint pad;
};

struct ToneMapOut {
    float4 position [[position]];
};

vertex ToneMapOut toneMapVertex(uint id [[vertex_id]]) {
    const float2 corner = float2(float((id << 1) & 2), float(id & 2)); // (0,0), (2,0), (0,2)
    ToneMapOut out;
    out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

// AgX, default look: the common minimal approximation of Troy Sobotka's AgX, returning linear sRGB.
float3 agx(float3 v) {
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float min_ev = -12.47393, max_ev = 4.026069;
    v = inset * v;
    v = clamp(log2(max(v, 1e-10)), min_ev, max_ev);
    v = (v - min_ev) / (max_ev - min_ev);
    const float3 x2 = v * v, x4 = x2 * x2;
    v = 15.5 * x4 * x2 - 40.14 * x4 * v + 31.96 * x4 - 6.868 * x2 * v + 0.4298 * x2 + 0.1191 * v - 0.00232;
    v = outset * v;
    return pow(max(v, 0.0), 2.2);
}

// Khronos PBR Neutral: base colors stay as authored up to about 0.76, then compress toward white.
float3 pbr_neutral(float3 c) {
    const float start = 0.8 - 0.04, desaturation = 0.15;
    const float x = min(c.r, min(c.g, c.b));
    c -= x < 0.08 ? x - 6.25 * x * x : 0.04;
    const float peak = max(c.r, max(c.g, c.b));
    if (peak < start) return c;
    const float d = 1.0 - start;
    const float new_peak = 1.0 - d * d / (peak + d - start);
    c *= new_peak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(c, float3(new_peak), g);
}

float3 srgb_encode(float3 c) {
    c = saturate(c);
    return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, 12.92 * c, c <= 0.0031308);
}

// False color by stops from middle grey (0.18), after exposure: blues under, grey around middle grey,
// yellow to red over, pink where the image is clipped.
float3 false_color(float stops) {
    if (stops < -6.0) return float3(0.25, 0.0, 0.45);
    if (stops < -4.0) return float3(0.0, 0.2, 0.9);
    if (stops < -2.0) return float3(0.0, 0.65, 0.8);
    if (stops < -0.5) return float3(0.2, 0.65, 0.25);
    if (stops <= 0.5) return float3(0.5);
    if (stops <= 2.0) return float3(0.85, 0.8, 0.2);
    if (stops <= 4.0) return float3(1.0, 0.55, 0.0);
    if (stops <= 6.0) return float3(0.9, 0.1, 0.1);
    return float3(1.0, 0.6, 0.9);
}

fragment float4 toneMapFragment(ToneMapOut in [[stage_in]], texture2d<float> scene [[texture(0)]],
                                constant ToneMapConstants& constants [[buffer(0)]]) {
    const float3 light = max(scene.read(uint2(in.position.xy)).rgb, 0.0) * constants.exposure;
    if (constants.view != 0) {
        const float luminance = dot(light, float3(0.2126, 0.7152, 0.0722)); // Rec. 709
        const float stops = log2(max(luminance, 1e-8) / 0.18);
        if (constants.view == 2) return float4(false_color(stops), 1.0);
        return float4(float3(saturate((stops + 8.0) / 16.0)), 1.0); // -8 stops black, +8 white
    }
    return float4(srgb_encode(constants.tone_mapping == 1 ? pbr_neutral(light) : agx(light)), 1.0);
}
