#pragma once
#include "maya/assets/asset_ref.hpp"
#include "maya/math/vector.hpp"
#include <cstdint>

namespace maya {
/// How a material's alpha (base color alpha times the base color map's) is used, as in glTF.
enum class AlphaMode : uint8_t {
    opaque, // ignored: the surface is opaque
    mask, // the surface is cut out where alpha is below the cutoff
    blend, // blended over what is behind it, drawn back to front after opaque surfaces
};
/// glTF's metallic-roughness material (docs/renderer.md#materials): each input is a factor, and an
/// optional texture that multiplies it. Colors are linear. Saved as a material file, version 2
/// (docs/assets.md#materials).
struct MaterialAsset {
    math::Vec4 base_color{1, 1, 1, 1}; // linear RGBA; multiplies the vertex color
    float metallic = 0;
    float roughness = 1; // perceptual: GGX's alpha is its square
    AssetRef<TextureAsset> base_color_texture{}; // color role (sRGB)
    AssetRef<TextureAsset> metallic_roughness_texture{}; // data role: roughness in green, metallic in blue
    AssetRef<TextureAsset> normal_texture{}; // normal role: tangent space, +Y up the texture
    float normal_scale = 1; // scales the map's x and y
    AssetRef<TextureAsset> occlusion_texture{}; // data role: red
    float occlusion_strength = 1; // 0 ignores the map
    math::Vec3 emissive{0, 0, 0}; // linear RGB, 0 to 1
    float emissive_strength = 1; // multiplies emissive, as KHR_materials_emissive_strength
    AssetRef<TextureAsset> emissive_texture{}; // color role (sRGB)
    AlphaMode alpha_mode = AlphaMode::opaque;
    float alpha_cutoff = 0.5f; // mask only
    bool double_sided = false; // drawn from behind too, with the normal flipped
};
inline bool operator==(const MaterialAsset& a, const MaterialAsset& b) noexcept {
    const auto same4 = [](const math::Vec4& x, const math::Vec4& y) { return x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w; };
    const auto same3 = [](const math::Vec3& x, const math::Vec3& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
    return same4(a.base_color, b.base_color) && a.metallic == b.metallic && a.roughness == b.roughness &&
           a.base_color_texture == b.base_color_texture && a.metallic_roughness_texture == b.metallic_roughness_texture &&
           a.normal_texture == b.normal_texture && a.normal_scale == b.normal_scale &&
           a.occlusion_texture == b.occlusion_texture && a.occlusion_strength == b.occlusion_strength &&
           same3(a.emissive, b.emissive) && a.emissive_strength == b.emissive_strength &&
           a.emissive_texture == b.emissive_texture && a.alpha_mode == b.alpha_mode &&
           a.alpha_cutoff == b.alpha_cutoff && a.double_sided == b.double_sided;
}
} // namespace maya
