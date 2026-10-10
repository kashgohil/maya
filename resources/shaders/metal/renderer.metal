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

struct LocalLight { // a point or spot light
    float4 position_range; // xyz, w range
    float4 direction_spot; // xyz a spot's direction, w 1 for a spot
    float4 intensity; // rgb candela, w its spot shadow map + 1 (0: none)
    float4 cone; // x cos outer, y 1 / (cos inner - cos outer), z bias and w normal bias in texels
};

struct Shadows {
    float4x4 cascades[4]; // world to each cascade's clip space
    float4 cascade_far; // the view depth where each cascade ends
    float4 cascade_texel; // metres per texel
    float4 sun; // x bias and y normal bias in texels, z the view's near plane, w the sun's index + 1 (0: none)
    float4x4 spots[4]; // world to each spot light's clip space
    float4 spot_texel; // metres per texel at one metre from each spot light
    float4 view_forward; // xyz the view's forward axis, w the DebugView (debug pipelines only)
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
    uint4 local_count; // x the point and spot lights drawn
    LocalLight local_lights[16];
    Shadows shadows;
};

struct DrawConstants { // per instance, for the vertex stage: an array indexed through the pass's order
    float4x4 model;
    float4 normal_matrix[3];
    uint4 skin; // x the first joint in the palette, y the skin's joints (0: not skinned), z the level of detail
};

// Skinned meshes' second vertex stream (#1038): four joints, as indices into the skin, and their weights.
struct SkinVertex {
    ushort4 joints;
    packed_float4 weights; // sum to 1
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
#ifdef MAYA_DEBUG_VIEWS
    uint level [[flat]]; // the level of detail drawn, for the LOD view
#endif
};

// A vertex placed by `model`, with `normal_matrix` the inverse transpose of its linear part, which keeps
// normals perpendicular under nonuniform scale; tangents lie in the surface, so the model matrix carries them.
LitOut lit_vertex(Vertex v, float4x4 model, float3x3 normal_matrix, constant ViewConstants& view) {
    const float4 world = model * float4(v.position, 1.0);
    const float3x3 linear = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    LitOut out;
    out.position = view.view_projection * world;
    out.world_position = world.xyz;
    out.world_normal = normal_matrix * v.normal;
    out.world_tangent = float4(linear * v.tangent.xyz, v.tangent.w);
    out.color = v.color;
    out.uv = v.uv;
    return out;
}

// Instanced draws (#1025): each instance number indexes the pass's order, which picks the instance's
// transform from the view's instances. Both are uploaded once per view and bound for every pass.
vertex LitOut litVertex(uint id [[vertex_id]], uint instance [[instance_id]],
                        constant Vertex* vertices [[buffer(0)]],
                        const device DrawConstants* instances [[buffer(1)]],
                        constant ViewConstants& view [[buffer(2)]],
                        const device uint* order [[buffer(4)]]) {
    const DrawConstants draw = instances[order[instance]];
    LitOut out = lit_vertex(vertices[id], draw.model, float3x3(draw.normal_matrix[0].xyz, draw.normal_matrix[1].xyz,
                                                               draw.normal_matrix[2].xyz), view);
#ifdef MAYA_DEBUG_VIEWS
    out.level = draw.skin.z;
#endif
    return out;
}

// Skinning (#1038, docs/animation.md#skinning): a vertex's world placement is its joints' skin matrices
// (each joint's world matrix times its inverse bind matrix) blended by its weights. Joint indices beyond
// the skin are clamped to its last joint, so a mesh and skin that disagree cannot read past the palette.
// An instance whose skin did not bind (skin.y 0) is placed by its own model matrix instead.
float4x4 skin_matrix(SkinVertex s, DrawConstants draw, const device float4x4* palette) {
    if (draw.skin.y == 0) return draw.model;
    const uint first = draw.skin.x, last = draw.skin.y - 1;
    const float4 w = float4(s.weights);
    return w.x * palette[first + min(uint(s.joints.x), last)] + w.y * palette[first + min(uint(s.joints.y), last)] +
           w.z * palette[first + min(uint(s.joints.z), last)] + w.w * palette[first + min(uint(s.joints.w), last)];
}
// The cofactor matrix: the inverse transpose times the determinant, which the fragment stage's
// normalization removes.
float3x3 cofactors(float4x4 m) {
    return float3x3(cross(m[1].xyz, m[2].xyz), cross(m[2].xyz, m[0].xyz), cross(m[0].xyz, m[1].xyz));
}
vertex LitOut litSkinnedVertex(uint id [[vertex_id]], uint instance [[instance_id]],
                               constant Vertex* vertices [[buffer(0)]],
                               const device DrawConstants* instances [[buffer(1)]],
                               constant ViewConstants& view [[buffer(2)]],
                               const device uint* order [[buffer(4)]],
                               const device SkinVertex* skins [[buffer(5)]],
                               const device float4x4* palette [[buffer(6)]]) {
    const DrawConstants draw = instances[order[instance]];
    const float4x4 model = skin_matrix(skins[id], draw, palette);
    const float3x3 normal_matrix = draw.skin.y == 0 ? float3x3(draw.normal_matrix[0].xyz, draw.normal_matrix[1].xyz,
                                                               draw.normal_matrix[2].xyz)
                                                    : cofactors(model);
    LitOut out = lit_vertex(vertices[id], model, normal_matrix, view);
#ifdef MAYA_DEBUG_VIEWS
    out.level = draw.skin.z;
#endif
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
// The specular's share of light from the surroundings (#1036), as glTF's Sample Renderer computes it: the
// split-sum table's scale and bias with a roughness-dependent Fresnel, plus Fdez-Aguera's multiple
// scattering, so rough metals keep the energy single scattering loses. tests/support/shading.hpp matches.
float3 surroundings_share(float3 f0, float2 scale_bias, float NdotV, float roughness) {
    const float3 k = f0 + (max(float3(1.0 - roughness), f0) - f0) * pow(1.0 - NdotV, 5.0);
    const float3 single = k * scale_bias.x + scale_bias.y;
    const float missing = 1.0 - (scale_bias.x + scale_bias.y);
    const float3 average = f0 + (1.0 - f0) / 21.0;
    return single + missing * single * average / (1.0 - average * missing);
}
// The equirectangular background's coordinates: u 0.5 along -Z, growing toward +X; v 0 straight up.
float2 equirect_uv(float3 d) {
    return float2(0.5 + atan2(d.x, -d.z) / (2.0 * M_PI_F), acos(clamp(d.y, -1.0, 1.0)) / M_PI_F);
}

// Texture slots, as MaterialSlot in include/maya/renderer/render_snapshot.hpp.
constant uint slot_base_color = 0, slot_metallic_roughness = 1, slot_normal = 2, slot_occlusion = 3, slot_emissive = 4;
bool has_map(constant MaterialConstants& material, uint slot) { return (material.flags.x >> slot & 1u) != 0; }

// Shadow maps (docs/renderer.md#shadows): depth from the light, written by a depth-only pipeline; masked
// materials cut themselves out as they do when lit.
struct ShadowConstants {
    float4x4 view_projection;
};
struct ShadowOut {
    float4 position [[position]];
    float2 uv;
    float alpha;
};
vertex ShadowOut shadowVertex(uint id [[vertex_id]], uint instance [[instance_id]], constant Vertex* vertices [[buffer(0)]],
                              const device DrawConstants* instances [[buffer(1)]], constant ShadowConstants& shadow [[buffer(2)]],
                              const device uint* order [[buffer(4)]]) {
    const Vertex v = vertices[id];
    const DrawConstants draw = instances[order[instance]];
    ShadowOut out;
    out.position = shadow.view_projection * (draw.model * float4(v.position, 1.0));
    out.uv = v.uv;
    out.alpha = v.color.a;
    return out;
}
vertex ShadowOut shadowSkinnedVertex(uint id [[vertex_id]], uint instance [[instance_id]], constant Vertex* vertices [[buffer(0)]],
                                     const device DrawConstants* instances [[buffer(1)]], constant ShadowConstants& shadow [[buffer(2)]],
                                     const device uint* order [[buffer(4)]], const device SkinVertex* skins [[buffer(5)]],
                                     const device float4x4* palette [[buffer(6)]]) {
    const Vertex v = vertices[id];
    const DrawConstants draw = instances[order[instance]];
    ShadowOut out;
    out.position = shadow.view_projection * (skin_matrix(skins[id], draw, palette) * float4(v.position, 1.0));
    out.uv = v.uv;
    out.alpha = v.color.a;
    return out;
}
fragment void shadowMaskFragment(ShadowOut in [[stage_in]], constant MaterialConstants& material [[buffer(3)]],
                                 texture2d<float> base_color_map [[texture(0)]], sampler base_color_sampler [[sampler(0)]]) {
    const float2 uv = float2(dot(material.uv_transform.xy, in.uv), dot(material.uv_transform.zw, in.uv)) + material.uv_offset.xy;
    float alpha = in.alpha * material.base_color.a;
    if (has_map(material, slot_base_color)) alpha *= base_color_map.sample(base_color_sampler, uv).a;
    if (alpha < material.emissive.w) discard_fragment();
}

// 3 x 3 comparison taps about a point in one map of an atlas, each filtered by the sampler: kept inside
// the map's square [lo, hi] so taps never read a neighbour.
float shadow_taps(depth2d<float> atlas, sampler s, float2 uv, float depth, float texel, float2 lo, float2 hi) {
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) lit += atlas.sample_compare(s, clamp(uv + float2(x, y) * texel, lo, hi), depth);
    return lit / 9.0;
}
// A map's quarter of a 2 x 2 atlas, from clip space.
float2 atlas_uv(float4 clip, uint map) { return float2(map & 1, map >> 1) * 0.5 + float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5) * 0.5; }

// The sun's light reaching P through cascade c: P is moved toward the light and along the surface's
// normal by the biases, in that cascade's texels.
float cascade_lit(constant ViewConstants& view, depth2d<float> atlas, sampler s, uint c, float3 P, float3 Ng, float3 L) {
    const float texel = view.shadows.cascade_texel[c];
    const float3 offset = P + (Ng * view.shadows.sun.y + L * view.shadows.sun.x) * texel;
    const float4 clip = view.shadows.cascades[c] * float4(offset, 1.0);
    const float atlas_texel = 1.0 / 4096.0;
    const float2 lo = float2(c & 1, c >> 1) * 0.5 + 1.5 * atlas_texel;
    return shadow_taps(atlas, s, atlas_uv(clip, c), clip.z, atlas_texel, lo, lo + 0.5 - 3.0 * atlas_texel);
}
// The cascade covering a view depth, and how far into its blend band toward the next one (0 to 1).
uint cascade_at(constant ViewConstants& view, float depth, thread float& blend) {
    uint c = 0;
    while (c < 3 && depth > view.shadows.cascade_far[c]) ++c;
    const float start = c == 0 ? view.shadows.sun.z : view.shadows.cascade_far[c - 1];
    const float band = view.shadows.cascade_far[c] - 0.1 * (view.shadows.cascade_far[c] - start);
    blend = saturate((depth - band) / (view.shadows.cascade_far[c] - band));
    return c;
}
float sun_lit(constant ViewConstants& view, depth2d<float> atlas, sampler s, float3 P, float3 Ng, float3 L) {
    const float depth = dot(P - view.camera_position.xyz, view.shadows.view_forward.xyz);
    if (depth >= view.shadows.cascade_far[3]) return 1.0;
    float blend;
    const uint c = cascade_at(view, depth, blend);
    float lit = cascade_lit(view, atlas, s, c, P, Ng, L);
    // Into the next cascade across the band, so no seam shows; past the last, the shadow fades out.
    if (blend > 0.0) lit = mix(lit, c < 3 ? cascade_lit(view, atlas, s, c + 1, P, Ng, L) : 1.0, blend);
    return lit;
}
float spot_lit(constant ViewConstants& view, depth2d<float> atlas, sampler s, uint map, float3 P, float3 Ng, float3 L,
               float distance, float bias, float normal_bias) {
    const float texel = distance * view.shadows.spot_texel[map];
    const float4 clip = view.shadows.spots[map] * float4(P + (Ng * normal_bias + L * bias) * texel, 1.0);
    if (clip.w <= 0.0) return 1.0;
    const float4 ndc = float4(clip.xyz / clip.w, 1.0);
    const float atlas_texel = 1.0 / 2048.0;
    const float2 lo = float2(map & 1, map >> 1) * 0.5 + 1.5 * atlas_texel;
    return shadow_taps(atlas, s, atlas_uv(ndc, map), ndc.z, atlas_texel, lo, lo + 0.5 - 3.0 * atlas_texel);
}

// What a surface sends toward V per unit of illuminance from L (glTF's BRDF), times N.L.
float3 direct_light(float3 N, float3 V, float3 L, float3 f0, float3 c_diff, float alpha, float NdotV) {
    const float NdotL = dot(N, L);
    if (NdotL <= 0.0) return float3(0.0);
    const float3 H = normalize(L + V);
    const float NdotH = saturate(dot(N, H)), VdotH = saturate(dot(V, H));
    const float3 F = fresnel_schlick(f0, VdotH);
    const float3 diffuse = (1.0 - F) * c_diff / M_PI_F;
    const float3 specular = F * ggx(NdotH, alpha) * smith_visibility(NdotL, NdotV, alpha);
    return (diffuse + specular) * NdotL;
}

float3 srgb_encode(float3 c) {
    c = saturate(c);
    return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, 12.92 * c, c <= 0.0031308);
}

// Debug views (docs/renderer.md#debug-views) are compiled only into the debug pipelines, which the renderer
// builds with MAYA_DEBUG_VIEWS defined when a view needs them; the lit pipelines never carry them.
#ifdef MAYA_DEBUG_VIEWS
constant uint debug_base_color = 3, debug_normals = 4, debug_shading_normals = 5, debug_metallic = 6, debug_roughness = 7,
              debug_occlusion = 8, debug_emissive = 9, debug_direct = 10, debug_environment = 11, debug_lighting = 12,
              debug_cascades = 13, debug_texels = 14, debug_lod = 15; // DebugView
#endif

/// What a surface is made of where a fragment lands: the material's inputs (a mesh's, or a terrain's blended
/// layers), its normals facing the viewer, and where it is.
struct Surface {
    float4 base; // rgb, and alpha
    float metallic, roughness; // as authored, before clamping
    float3 N; // shading normal
    float3 Ng; // geometric normal, for shadow offsets
    float occlusion;
    float3 emitted;
    float3 world_position;
    uint alpha_mode;
    uint level; // of detail, for the LOD view
};

/// Lights a surface: the surroundings, the lights, and what it emits; or, in the debug pipelines, the view
/// chosen. Mesh and terrain fragments share it (#1068).
float4 shade(Surface s, constant ViewConstants& view, texturecube<float> specular_cube, sampler environment_sampler,
             texture2d<float> brdf_table, sampler table_sampler, depth2d<float> sun_shadows, depth2d<float> spot_shadows,
             sampler shadow_sampler) {
#ifdef MAYA_DEBUG_VIEWS
    const uint debug_view = uint(view.shadows.view_forward.w);
    float3 base_rgb = s.base.rgb;
    float metallic = s.metallic, roughness = s.roughness;
    const float authored_roughness = roughness;
    // Lighting without albedo: white, fully rough, and not metallic; the LOD view tints that.
    if (debug_view == debug_lighting || debug_view == debug_lod) {
        base_rgb = float3(1.0);
        metallic = 0.0;
        roughness = 1.0;
    }
    const bool with_surroundings = debug_view != debug_direct, with_direct = debug_view != debug_environment;
    const bool with_emitted = (debug_view < debug_direct || debug_view > debug_lighting) && debug_view != debug_lod;
#else
    float3 base_rgb = s.base.rgb;
    float metallic = s.metallic, roughness = s.roughness;
    constexpr bool with_surroundings = true, with_direct = true, with_emitted = true;
#endif
    metallic = saturate(metallic);
    roughness = clamp(roughness, min_roughness, 1.0);
    const float alpha = roughness * roughness;

    const float3 N = s.N, Ng = s.Ng;
    const float3 base = base_rgb;
    const float3 c_diff = base * (1.0 - metallic);
    const float3 f0 = mix(float3(0.04), base, metallic);
    const float3 V = normalize(view.camera_position.xyz - s.world_position);
    const float NdotV = max(dot(N, V), 1e-4);

    // Light from the surroundings: the environment's, or the uniform ambient light. As glTF defines a
    // material, a dielectric's result (diffuse, less the specular's share, plus that share of the specular)
    // and a metal's are mixed by metallic. Occlusion darkens both.
    const float occlusion = s.occlusion;
    const float2 scale_bias = brdf_table.sample(table_sampler, table_uv(float2(NdotV, roughness))).rg;
    float3 diffuse_light = view.ambient.rgb, specular_light = view.ambient.rgb;
    if (view.environment_flags.x != 0) {
        const float intensity = view.environment.x;
        diffuse_light = environment_irradiance(view, to_environment(view, N)) / M_PI_F * intensity;
        const float3 R = to_environment(view, reflect(-V, N));
        specular_light = specular_cube.sample(environment_sampler, R, level(roughness * view.environment.w)).rgb * intensity;
    }
    const float3 dielectric_share = surroundings_share(float3(0.04), scale_bias, NdotV, roughness);
    const float3 metal_share = surroundings_share(base, scale_bias, NdotV, roughness);
    const float3 dielectric = (1.0 - dielectric_share) * base * diffuse_light + dielectric_share * specular_light;
    float3 rgb = with_surroundings ? mix(dielectric, metal_share * specular_light, metallic) * occlusion : float3(0.0);

    // Directional lights: illuminance in lux; the sun is shadowed by its cascades.
    const uint lights = min(view.light_count.x, 4u);
    const uint sun = uint(view.shadows.sun.w);
    for (uint i = 0; with_direct && i < lights; ++i) {
        const float3 L = view.lights[i].direction_to_light.xyz;
        float lit = 1.0;
        if (i + 1 == sun && dot(N, L) > 0.0) lit = sun_lit(view, sun_shadows, shadow_sampler, s.world_position, Ng, L);
        rgb += view.lights[i].radiance.rgb * lit * direct_light(N, V, L, f0, c_diff, alpha, NdotV);
    }
    // Point and spot lights (docs/renderer.md#lights): candela / d^2, faded to nothing at the range as glTF
    // recommends, and a spot's between its cones.
    const uint locals = min(view.local_count.x, 16u);
    for (uint i = 0; with_direct && i < locals; ++i) {
        const LocalLight light = view.local_lights[i];
        float3 L = light.position_range.xyz - s.world_position;
        const float distance = length(L);
        L /= max(distance, 1e-6);
        const float window = saturate(1.0 - pow(distance / light.position_range.w, 4.0));
        float falloff = window / max(distance * distance, 1e-8);
        if (light.direction_spot.w > 0.0) {
            const float t = saturate((dot(-L, light.direction_spot.xyz) - light.cone.x) * light.cone.y);
            falloff *= t * t;
        }
        if (falloff <= 0.0 || dot(N, L) <= 0.0) continue;
        if (light.intensity.w > 0.0)
            falloff *= spot_lit(view, spot_shadows, shadow_sampler, uint(light.intensity.w) - 1, s.world_position, Ng, L,
                                distance, light.cone.z, light.cone.w);
        rgb += light.intensity.rgb * falloff * direct_light(N, V, L, f0, c_diff, alpha, NdotV);
    }

    const float3 emitted = s.emitted;
    if (with_emitted) rgb += emitted;

#ifdef MAYA_DEBUG_VIEWS
    // Material inputs are written as display values, which the tone-mapping pass shows unchanged: colors
    // sRGB-encoded as authored, and data as grey or as 0.5 + 0.5 n.
    const float out_alpha = s.alpha_mode == 2 ? s.base.a : 1.0;
    switch (debug_view) {
    case debug_base_color: return float4(srgb_encode(base), out_alpha);
    case debug_normals: return float4(Ng * 0.5 + 0.5, out_alpha);
    case debug_shading_normals: return float4(N * 0.5 + 0.5, out_alpha);
    case debug_metallic: return float4(float3(metallic), out_alpha);
    case debug_roughness: return float4(float3(saturate(authored_roughness)), out_alpha);
    case debug_occlusion: return float4(float3(occlusion), out_alpha);
    case debug_emissive: return float4(srgb_encode(emitted), out_alpha);
    default: break;
    }
    // Shadow views: the sun's cascade tints each surface, and a checker shows its shadow-map texels.
    if ((debug_view == debug_cascades || debug_view == debug_texels) && sun != 0) {
        const float depth = dot(s.world_position - view.camera_position.xyz, view.shadows.view_forward.xyz);
        if (depth < view.shadows.cascade_far[3]) {
            float blend;
            const uint c = cascade_at(view, depth, blend);
            const float3 tints[4] = {float3(1.0, 0.35, 0.35), float3(0.35, 1.0, 0.35), float3(0.35, 0.5, 1.0), float3(1.0, 1.0, 0.35)};
            rgb *= tints[c] * 1.5;
            if (debug_view == debug_texels) {
                const float4 clip = view.shadows.cascades[c] * float4(s.world_position, 1.0);
                const float2 texel = floor(atlas_uv(clip, c) * 4096.0);
                if ((int(texel.x) + int(texel.y)) & 1) rgb *= 0.55;
            }
        }
    }
    // Levels of detail (#1066): the finest green, then yellow, orange, red, magenta, and blue the coarsest.
    if (debug_view == debug_lod) {
        const float3 tints[6] = {float3(0.35, 1.0, 0.35), float3(1.0, 1.0, 0.3), float3(1.0, 0.6, 0.25),
                                 float3(1.0, 0.3, 0.3), float3(1.0, 0.35, 1.0), float3(0.4, 0.5, 1.0)};
        rgb *= tints[min(s.level, 5u)] * 1.5;
    }
#endif
    return float4(rgb, s.alpha_mode == 2 ? s.base.a : 1.0);
}

fragment float4 litFragment(LitOut in [[stage_in]], bool front [[front_facing]],
                            constant ViewConstants& view [[buffer(2)]],
                            constant MaterialConstants& material [[buffer(3)]],
                            texture2d<float> base_color_map [[texture(0)]], sampler base_color_sampler [[sampler(0)]],
                            texture2d<float> metallic_roughness_map [[texture(1)]], sampler metallic_roughness_sampler [[sampler(1)]],
                            texture2d<float> normal_map [[texture(2)]], sampler normal_sampler [[sampler(2)]],
                            texture2d<float> occlusion_map [[texture(3)]], sampler occlusion_sampler [[sampler(3)]],
                            texture2d<float> emissive_map [[texture(4)]], sampler emissive_sampler [[sampler(4)]],
                            texturecube<float> specular_cube [[texture(5)]], sampler environment_sampler [[sampler(5)]],
                            texture2d<float> brdf_table [[texture(7)]], sampler table_sampler [[sampler(7)]],
                            depth2d<float> sun_shadows [[texture(8)]], depth2d<float> spot_shadows [[texture(9)]],
                            sampler shadow_sampler [[sampler(8)]]) {
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

    Surface surface;
    surface.base = base;
    surface.metallic = metallic;
    surface.roughness = roughness;
    surface.N = N;
    surface.Ng = normalize(in.world_normal) * (front ? 1.0 : -1.0); // for shadow offsets
    surface.occlusion = 1.0;
    if (has_map(material, slot_occlusion))
        surface.occlusion = 1.0 + material.factors.w * (occlusion_map.sample(occlusion_sampler, uv).r - 1.0);
    surface.emitted = material.emissive.rgb;
    if (has_map(material, slot_emissive)) surface.emitted *= emissive_map.sample(emissive_sampler, uv).rgb;
    surface.world_position = in.world_position;
    surface.alpha_mode = alpha_mode;
#ifdef MAYA_DEBUG_VIEWS
    surface.level = in.level;
#else
    surface.level = 0;
#endif
    return shade(surface, view, specular_cube, environment_sampler, brdf_table, table_sampler, sun_shadows, spot_shadows, shadow_sampler);
}

// Terrain (#1068, docs/renderer.md#terrain) -------------------------------------------------------------------
// CDLOD patches of 32 x 32 quads, placed by instance; each vertex reads its height from a tile's packed
// heights (buffer 5) or the terrain's overview (buffer 6), and morphs toward the next coarser grid near the
// end of its level's range, so neighbouring levels meet without cracks. Layout matches TerrainConstants and
// TerrainPatch in include/maya/renderer/shader_constants.hpp.
struct TerrainLayer {
    float4 base_color;
    float4 factors; // x metallic, y roughness, z normal scale, w occlusion strength
    uint4 flags; // x a bit per map in use (slot_*), y 1 when the layer is used
};
struct TerrainConstants {
    TerrainLayer layers[4];
    float4 slope[3]; // layers 1 to 3: x the slope (degrees) where it starts, y where it is whole
    float4 height[3]; // ... and the same by height (metres)
    float4 params; // x 1 / texture size, yz the texture coordinates of the anchor, w tile samples a side
    float4 overview; // xy the anchor (the region's low corner, the overview's sample (0, 0)) in the snapshot's frame, z what moves a stored height into it
    float4 overview_size; // x samples along x, y along z, z spacing (metres)
    float4 camera; // xyz the view's camera (shadow passes measure from it too); w its height above or below the terrain's band
    float4 region; // xy its low corner and zw its high one, in the snapshot's frame: outside, nothing is drawn
};
// Patches and tiles are placed in whole metres from the anchor, so a vertex two patches share has the same
// bits in both before the anchor's offset is added: there are no pinholes along their edge.
struct TerrainPatch {
    float4 origin_size; // xy its corner in metres from the anchor, z its size (metres), w its level
    float4 morph; // x the distance its morph starts, y ends, z its quad (metres), w 1 when its heights blend to the overview
    float4 source; // xy the tile's sample (0, 0) in metres from the anchor
    uint4 flags; // x 1 when it reads a tile (buffer 5), 0 for the overview
};
constant uint terrain_quads = 32;

// Packed heights: two words a sample, the height (a float) and the normal (octahedral, 8 bits of x and of z).
float tile_height(const device uint* tile, uint samples, int2 at) {
    const int2 c = clamp(at, int2(0), int2(int(samples) - 1));
    return as_type<float>(tile[2 * (uint(c.y) * samples + uint(c.x))]);
}
float3 decode_normal(uint word) {
    const float2 p = float2(float(word & 0xffu), float((word >> 8) & 0xffu)) / 255.0 * 2.0 - 1.0;
    float3 n = float3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    if (n.y < 0.0) n.xz = (1.0 - abs(n.zx)) * select(float2(-1.0), float2(1.0), n.xz >= 0.0);
    return normalize(n);
}
/// Bilinear over a grid of packed samples, `spacing` metres apart from `origin`.
struct GridSample {
    uint i00, i10, i01, i11;
    float2 f;
};
GridSample grid_at(float2 xz, float2 origin, float spacing, uint width, uint depth) {
    const float2 g = clamp((xz - origin) / spacing, float2(0.0), float2(float(width - 1), float(depth - 1)));
    const uint2 i = min(uint2(floor(g)), uint2(width - 2, depth - 2));
    GridSample s;
    s.f = g - float2(i);
    s.i00 = i.y * width + i.x;
    s.i10 = s.i00 + 1;
    s.i01 = s.i00 + width;
    s.i11 = s.i01 + 1;
    return s;
}
float overview_height(const device uint* overview, constant TerrainConstants& t, float2 local) {
    const GridSample s = grid_at(local, float2(0.0), t.overview_size.z, uint(t.overview_size.x), uint(t.overview_size.y));
    const float h00 = as_type<float>(overview[2 * s.i00]), h10 = as_type<float>(overview[2 * s.i10]);
    const float h01 = as_type<float>(overview[2 * s.i01]), h11 = as_type<float>(overview[2 * s.i11]);
    return mix(mix(h00, h10, s.f.x), mix(h01, h11, s.f.x), s.f.y);
}
float3 grid_normal(const device uint* words, GridSample s) {
    return normalize(mix(mix(decode_normal(words[2 * s.i00 + 1]), decode_normal(words[2 * s.i10 + 1]), s.f.x),
                         mix(decode_normal(words[2 * s.i01 + 1]), decode_normal(words[2 * s.i11 + 1]), s.f.x), s.f.y));
}
/// A patch's height at a point (metres from the anchor): its tile's sample there (patches lie on whole metres),
/// or the overview's.
float patch_height(TerrainPatch p, constant TerrainConstants& t, const device uint* tile, const device uint* overview, float2 local) {
    if (p.flags.x != 0) return tile_height(tile, uint(t.params.w), int2(round(local - p.source.xy)));
    return overview_height(overview, t, local);
}
/// A patch's vertex: where it lands after morphing (in the snapshot's frame, and from the anchor), and how far
/// it has morphed.
struct TerrainPoint {
    float3 position;
    float2 local;
    float morph;
};
TerrainPoint terrain_point(uint id, TerrainPatch p, constant TerrainConstants& t, const device uint* tile, const device uint* overview) {
    const uint2 g = uint2(id % (terrain_quads + 1), id / (terrain_quads + 1));
    const float quad = p.morph.z;
    float2 local = p.origin_size.xy + float2(g) * quad; // exact: whole metres
    // The distance before morphing decides how far to morph: horizontal, with the terrain's height gap, as
    // selection measures it, so both sides of an edge agree and the levels' ranges hold (docs/renderer.md#terrain).
    const float2 across = local + t.overview.xy - t.camera.xz;
    const float d = sqrt(dot(across, across) + t.camera.w * t.camera.w);
    const float k = saturate((d - p.morph.x) / max(p.morph.y - p.morph.x, 1e-3));
    // Odd vertices onto the coarser grid; fully there (k is exactly 1), exactly: whole metres again.
    local -= fmod(float2(g), 2.0) * quad * k;
    float h = patch_height(p, t, tile, overview, local);
    if (p.morph.w != 0.0) { // toward the overview the next level reads; exactly it when fully morphed
        const float coarse = overview_height(overview, t, local);
        h = k >= 1.0 ? coarse : mix(h, coarse, k); // mix(h, coarse, 1) may round away from coarse
    }
    TerrainPoint point;
    point.position = float3(local.x + t.overview.x, h + t.overview.z, local.y + t.overview.y);
    point.local = local;
    point.morph = p.morph.w != 0.0 ? k : 0.0;
    return point;
}

struct TerrainOut {
    float4 position [[position]];
    float3 world_position;
    float2 local; // metres from the anchor
    float morph; // toward the overview's normals, where the heights blend
    float4 source [[flat]];
    uint from_tile [[flat]];
    uint level [[flat]];
};
vertex TerrainOut terrainVertex(uint id [[vertex_id]], uint instance [[instance_id]], const device TerrainPatch* patches [[buffer(1)]],
                                constant ViewConstants& view [[buffer(2)]], constant TerrainConstants& terrain [[buffer(3)]],
                                const device uint* tile [[buffer(5)]], const device uint* overview [[buffer(6)]]) {
    const TerrainPatch p = patches[instance];
    const TerrainPoint point = terrain_point(id, p, terrain, tile, overview);
    TerrainOut out;
    out.position = view.view_projection * float4(point.position, 1.0);
    out.world_position = point.position;
    out.local = point.local;
    out.morph = point.morph;
    out.source = p.source;
    out.from_tile = p.flags.x;
    out.level = uint(p.origin_size.w);
    return out;
}

struct TerrainShadowOut {
    float4 position [[position]];
    float2 xz;
};
vertex TerrainShadowOut terrainShadowVertex(uint id [[vertex_id]], uint instance [[instance_id]], const device TerrainPatch* patches [[buffer(1)]],
                                            constant ShadowConstants& shadow [[buffer(2)]], constant TerrainConstants& terrain [[buffer(3)]],
                                            const device uint* tile [[buffer(5)]], const device uint* overview [[buffer(6)]]) {
    const TerrainPoint point = terrain_point(id, patches[instance], terrain, tile, overview);
    TerrainShadowOut out;
    out.position = shadow.view_projection * float4(point.position, 1.0);
    out.xz = point.position.xz;
    return out;
}
bool outside_region(constant TerrainConstants& t, float2 xz) {
    return any(xz < t.region.xy) || any(xz > t.region.zw);
}
fragment void terrainShadowFragment(TerrainShadowOut in [[stage_in]], constant TerrainConstants& terrain [[buffer(3)]]) {
    if (outside_region(terrain, in.xz)) discard_fragment();
}

/// From none of a layer at r.x to all of it at r.y (a step there when they are equal).
float ramp(float value, float2 r) {
    return r.x == r.y ? step(r.x, value) : saturate((value - r.x) / (r.y - r.x));
}
fragment float4 terrainFragment(TerrainOut in [[stage_in]], constant ViewConstants& view [[buffer(2)]],
                                constant TerrainConstants& terrain [[buffer(3)]],
                                const device uint* tile [[buffer(5)]], const device uint* overview [[buffer(6)]],
                                array<texture2d<float>, 16> layer_maps [[texture(10)]], sampler layer_sampler [[sampler(9)]],
                                texturecube<float> specular_cube [[texture(5)]], sampler environment_sampler [[sampler(5)]],
                                texture2d<float> brdf_table [[texture(7)]], sampler table_sampler [[sampler(7)]],
                                depth2d<float> sun_shadows [[texture(8)]], depth2d<float> spot_shadows [[texture(9)]],
                                sampler shadow_sampler [[sampler(8)]]) {
    const float2 xz = in.world_position.xz;
    if (outside_region(terrain, xz)) discard_fragment();
    // The surface's normal from the heights' normals: the tile's, blended toward the overview's where the
    // heights blend; the overview's alone where it is drawn from.
    const GridSample coarse = grid_at(in.local, float2(0.0), terrain.overview_size.z, uint(terrain.overview_size.x), uint(terrain.overview_size.y));
    float3 Ng = grid_normal(overview, coarse);
    if (in.from_tile != 0) {
        const uint samples = uint(terrain.params.w);
        const float3 fine = grid_normal(tile, grid_at(in.local, in.source.xy, 1.0, samples, samples));
        Ng = normalize(mix(fine, Ng, in.morph));
    }
    // Layers over the ground by slope and height, each with its maps, mapped across x and z.
    const float slope = acos(saturate(Ng.y)) * (180.0 / M_PI_F);
    const float2 uv = in.local * terrain.params.x + terrain.params.yz;
    const float3 T = normalize(float3(1.0, 0.0, 0.0) - Ng * Ng.x); // along +x, in the surface
    const float3 B = cross(T, Ng); // along +z
    float4 base = float4(0.0, 0.0, 0.0, 1.0);
    float metallic = 0.0, roughness = 1.0, occlusion = 1.0;
    float3 N = Ng;
    for (uint i = 0; i < 4; ++i) {
        const TerrainLayer layer = terrain.layers[i];
        if (layer.flags.y == 0) continue;
        const float weight = i == 0 ? 1.0 : ramp(slope, terrain.slope[i - 1].xy) * ramp(in.world_position.y, terrain.height[i - 1].xy);
        if (weight <= 0.0) continue;
        float4 color = layer.base_color;
        if ((layer.flags.x >> slot_base_color & 1u) != 0) color *= layer_maps[i * 4 + 0].sample(layer_sampler, uv);
        float m = layer.factors.x, r = layer.factors.y;
        if ((layer.flags.x >> slot_metallic_roughness & 1u) != 0) {
            const float4 sample = layer_maps[i * 4 + 1].sample(layer_sampler, uv);
            r *= sample.g;
            m *= sample.b;
        }
        float3 n = Ng;
        if ((layer.flags.x >> slot_normal & 1u) != 0) {
            const float4 sample = layer_maps[i * 4 + 2].sample(layer_sampler, uv);
            const float2 v = float2(sample.r, sample.a) * 2.0 - 1.0;
            const float3 t = normalize(float3(v * layer.factors.z, sqrt(saturate(1.0 - dot(v, v)))));
            n = normalize(T * t.x + B * t.y + Ng * t.z);
        }
        float o = 1.0;
        if ((layer.flags.x >> slot_occlusion & 1u) != 0) o = 1.0 + layer.factors.w * (layer_maps[i * 4 + 3].sample(layer_sampler, uv).r - 1.0);
        base = mix(base, color, weight);
        metallic = mix(metallic, m, weight);
        roughness = mix(roughness, r, weight);
        occlusion = mix(occlusion, o, weight);
        N = normalize(mix(N, n, weight));
    }
    Surface surface;
    surface.base = float4(base.rgb, 1.0);
    surface.metallic = metallic;
    surface.roughness = roughness;
    surface.N = N;
    surface.Ng = Ng;
    surface.occlusion = occlusion;
    surface.emitted = float3(0.0);
    surface.world_position = in.world_position;
    surface.alpha_mode = 0;
    surface.level = in.level;
    return shade(surface, view, specular_cube, environment_sampler, brdf_table, table_sampler, sun_shadows, spot_shadows, shadow_sampler);
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
    if (constants.view == 3) return float4(saturate(scene.read(uint2(in.position.xy)).rgb), 1.0); // material debug views
    const float3 light = max(scene.read(uint2(in.position.xy)).rgb, 0.0) * constants.exposure;
    if (constants.view != 0) {
        const float luminance = dot(light, float3(0.2126, 0.7152, 0.0722)); // Rec. 709
        const float stops = log2(max(luminance, 1e-8) / 0.18);
        if (constants.view == 2) return float4(false_color(stops), 1.0);
        return float4(float3(saturate((stops + 8.0) / 16.0)), 1.0); // -8 stops black, +8 white
    }
    return float4(srgb_encode(constants.tone_mapping == 1 ? pbr_neutral(light) : agx(light)), 1.0);
}
