// glTF 2.0 files (#1036, docs/import.md), read with cgltf and converted to Maya's data. This is the
// only translation unit that sees cgltf's types, and it holds cgltf's implementation.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "maya/assets/gltf.hpp"
#include "maya/core/tangents.hpp"
#include "maya/properties/schema.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace maya {
struct GltfFile::Data {
    cgltf_data* data = nullptr;
    std::filesystem::path folder; // where the file's relative URIs start
    ~Data() { cgltf_free(data); }
};
GltfFile::GltfFile(std::unique_ptr<Data> data, GltfDocument document) : m_data(std::move(data)), m_document(std::move(document)) {}
GltfFile::~GltfFile() = default;

std::string gltf_problem_text(const GltfProblem& problem) {
    return problem.path.empty() ? problem.message : problem.path + ": " + problem.message;
}

namespace {
std::string text(const char* value) { return value ? value : ""; }
std::string at(std::string_view array, size_t index) { return std::string(array) + "[" + std::to_string(index) + "]"; }

const char* result_text(cgltf_result result) {
    switch (result) {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "the data is shorter than it says";
    case cgltf_result_unknown_format: return "it is neither glTF JSON nor GLB";
    case cgltf_result_invalid_json: return "its JSON is invalid";
    case cgltf_result_invalid_gltf: return "it is not valid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "the file is missing";
    case cgltf_result_io_error: return "it cannot be read";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "it is glTF 1.0, which Maya does not read; convert it to glTF 2.0";
    default: return "unknown error";
    }
}
bool supported(std::string_view extension) {
    return std::ranges::any_of(gltf_supported_extensions, [&](const char* name) { return extension == name; });
}

math::Quat rotation_from(const math::Vec3& x, const math::Vec3& y, const math::Vec3& z) {
    // Columns of a rotation matrix: m[row][column].
    const float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
    auto q = math::Quat{};
    if (const auto trace = m00 + m11 + m22; trace > 0) {
        const auto s = std::sqrt(trace + 1) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const auto s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const auto s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const auto s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    q.normalize();
    return q;
}

/// A node's local transform as translation, rotation, and positive scale. A matrix, or a scale with a
/// negative component, is decomposed; a mirror (negative determinant) is dropped and a shear is lost.
TransformComponent node_transform(const cgltf_node& node, const std::string& path, std::vector<GltfProblem>& warnings) {
    auto transform = TransformComponent{};
    if (!node.has_matrix && (!node.has_scale || (node.scale[0] >= 0 && node.scale[1] >= 0 && node.scale[2] >= 0))) {
        if (node.has_translation) transform.translation = {node.translation[0], node.translation[1], node.translation[2]};
        if (node.has_rotation) {
            transform.rotation = {node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]};
            const auto length = std::sqrt(transform.rotation.x * transform.rotation.x + transform.rotation.y * transform.rotation.y +
                                          transform.rotation.z * transform.rotation.z + transform.rotation.w * transform.rotation.w);
            if (length > 0) transform.rotation.normalize();
            else transform.rotation = {};
        }
        if (node.has_scale) transform.scale = {node.scale[0], node.scale[1], node.scale[2]};
        return transform;
    }
    float m[16];
    cgltf_node_transform_local(&node, m);
    auto x = math::Vec3{m[0], m[1], m[2]}, y = math::Vec3{m[4], m[5], m[6]}, z = math::Vec3{m[8], m[9], m[10]};
    transform.translation = {m[12], m[13], m[14]};
    if (math::Vec3::dot(math::Vec3::cross(x, y), z) < 0) {
        x = -x;
        warnings.push_back({path, "is mirrored (a negative scale), which Maya's transforms cannot be; it is imported unmirrored"});
    }
    auto scale = math::Vec3{x.length(), y.length(), z.length()};
    if (scale.x <= 0 || scale.y <= 0 || scale.z <= 0) { // flattened: no rotation can be recovered from it
        transform.scale = scale;
        return transform;
    }
    const auto ux = x / scale.x, uy = y / scale.y, uz = z / scale.z;
    constexpr auto skew = 1e-3f;
    if (std::abs(math::Vec3::dot(ux, uy)) > skew || std::abs(math::Vec3::dot(uy, uz)) > skew || std::abs(math::Vec3::dot(ux, uz)) > skew)
        warnings.push_back({path, "has a shear, which a translation, rotation, and scale cannot hold; the shear is lost"});
    // Gram-Schmidt keeps the rotation a rotation, whatever shear remains.
    const auto rx = ux.normalized();
    const auto ry = (uy - rx * math::Vec3::dot(rx, uy)).normalized();
    const auto rz = math::Vec3::cross(rx, ry);
    transform.rotation = rotation_from(rx, ry, rz);
    transform.scale = scale;
    return transform;
}

SamplerDesc sampler_from(const cgltf_sampler* sampler) {
    auto desc = SamplerDesc{Filter::linear, Filter::linear, AddressMode::repeat, AddressMode::repeat, {}, MipFilter::linear, 1};
    if (!sampler) return desc;
    const auto wrap = [](cgltf_wrap_mode mode) {
        return mode == cgltf_wrap_mode_clamp_to_edge ? AddressMode::clamp_to_edge
             : mode == cgltf_wrap_mode_mirrored_repeat ? AddressMode::mirror_repeat : AddressMode::repeat;
    };
    desc.address_u = wrap(sampler->wrap_s);
    desc.address_v = wrap(sampler->wrap_t);
    desc.mag_filter = sampler->mag_filter == cgltf_filter_type_nearest ? Filter::nearest : Filter::linear;
    switch (sampler->min_filter) {
    case cgltf_filter_type_nearest: desc.min_filter = Filter::nearest; desc.mip_filter = MipFilter::none; break;
    case cgltf_filter_type_linear: desc.mip_filter = MipFilter::none; break;
    case cgltf_filter_type_nearest_mipmap_nearest: desc.min_filter = Filter::nearest; desc.mip_filter = MipFilter::nearest; break;
    case cgltf_filter_type_linear_mipmap_nearest: desc.mip_filter = MipFilter::nearest; break;
    case cgltf_filter_type_nearest_mipmap_linear: desc.min_filter = Filter::nearest; break;
    default: break; // linear, mipmap linear: also glTF's choice for an undefined filter
    }
    return desc;
}

/// The schema-valid material nearest to `wanted`: each property that the schema rejects keeps its
/// default, with a warning.
MaterialAsset valid_material(const MaterialAsset& wanted, const std::string& path, std::vector<GltfProblem>& warnings) {
    auto material = MaterialAsset{};
    for (const auto& property : material_properties()) {
        const auto value = read_property(wanted, property.id);
        if (!value) continue;
        const auto edit = PropertyEdit{property.id, *value};
        if (const auto edited = edit_properties(material, std::span(&edit, 1)); !edited)
            warnings.push_back({path, std::string(property.name) + ": " + std::string(edited.message) + "; the default is used"});
    }
    return material;
}

struct Converter {
    const cgltf_data& data;
    GltfDocument document;
    std::vector<GltfProblem> errors;

    void warn(std::string path, std::string message) { document.warnings.push_back({std::move(path), std::move(message)}); }
    void fail(std::string path, std::string message) { errors.push_back({std::move(path), std::move(message)}); }

    std::optional<GltfTextureUse> texture_use(const cgltf_texture_view& view, const std::string& path) {
        if (!view.texture) return std::nullopt;
        auto use = GltfTextureUse{uint32_t(cgltf_texture_index(&data, view.texture))};
        use.texcoord = uint32_t(std::max(0, view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord));
        if (use.texcoord != 0)
            warn(path, "uses TEXCOORD_" + std::to_string(use.texcoord) + "; Maya reads only TEXCOORD_0, so it uses that");
        if (view.has_transform) {
            use.transformed = true;
            use.offset = {view.transform.offset[0], view.transform.offset[1]};
            use.scale = {view.transform.scale[0], view.transform.scale[1]};
            use.rotation = view.transform.rotation;
            if (!std::isfinite(use.offset.x) || !std::isfinite(use.offset.y) || !std::isfinite(use.scale.x) ||
                !std::isfinite(use.scale.y) || !std::isfinite(use.rotation)) {
                warn(path + ".extensions.KHR_texture_transform", "has a value that is not finite; the texture is used untransformed");
                use.transformed = false;
                use.offset = math::Vec2{0.0f};
                use.scale = math::Vec2{1.0f};
                use.rotation = 0;
            }
        }
        return use;
    }

    void extensions() {
        for (size_t i = 0; i < data.extensions_required_count; ++i)
            if (!supported(data.extensions_required[i]))
                fail(at("extensionsRequired", i), std::string("requires ") + data.extensions_required[i] + ", which Maya does not read");
        for (size_t i = 0; i < data.extensions_used_count; ++i)
            if (!supported(data.extensions_used[i]))
                warn(at("extensionsUsed", i), std::string(data.extensions_used[i]) + " is not read; what it adds is left out");
    }

    void images() {
        for (size_t i = 0; i < data.images_count; ++i) {
            const auto& image = data.images[i];
            auto uri = text(image.uri);
            if (uri.starts_with("data:")) uri.clear();
            else if (!uri.empty()) uri.resize(cgltf_decode_uri(uri.data()));
            document.images.push_back({text(image.name), std::move(uri), text(image.mime_type)});
        }
    }
    void files() {
        const auto add = [&](std::string uri) {
            if (uri.empty() || uri.starts_with("data:")) return;
            uri.resize(cgltf_decode_uri(uri.data()));
            if (std::ranges::find(document.files, uri) == document.files.end()) document.files.push_back(std::move(uri));
        };
        for (size_t i = 0; i < data.buffers_count; ++i) add(text(data.buffers[i].uri));
        for (size_t i = 0; i < data.images_count; ++i) add(text(data.images[i].uri));
    }

    void textures() {
        for (size_t i = 0; i < data.textures_count; ++i) {
            const auto& texture = data.textures[i];
            auto converted = GltfTexture{text(texture.name), std::nullopt, sampler_from(texture.sampler)};
            if (texture.image) converted.image = uint32_t(cgltf_image_index(&data, texture.image));
            else if (texture.has_webp || texture.has_basisu)
                warn(at("textures", i), std::string("has only ") + (texture.has_webp ? "an EXT_texture_webp" : "a KHR_texture_basisu") +
                                            " image, which Maya does not read; it is left out");
            else warn(at("textures", i), "has no image; it is left out");
            document.textures.push_back(std::move(converted));
        }
    }

    void materials() {
        for (size_t i = 0; i < data.materials_count; ++i) {
            const auto& source = data.materials[i];
            const auto path = at("materials", i);
            const auto& pbr = source.pbr_metallic_roughness;
            auto wanted = MaterialAsset{};
            wanted.base_color = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3]};
            wanted.metallic = pbr.metallic_factor;
            wanted.roughness = pbr.roughness_factor;
            wanted.normal_scale = source.normal_texture.scale;
            wanted.occlusion_strength = source.occlusion_texture.scale;
            wanted.emissive = {source.emissive_factor[0], source.emissive_factor[1], source.emissive_factor[2]};
            if (source.has_emissive_strength) wanted.emissive_strength = source.emissive_strength.emissive_strength;
            wanted.alpha_mode = source.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::mask
                              : source.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::blend : AlphaMode::opaque;
            wanted.alpha_cutoff = source.alpha_cutoff;
            wanted.double_sided = source.double_sided;
            if (!source.has_pbr_metallic_roughness && source.has_pbr_specular_glossiness)
                warn(path, "has only specular-glossiness values, which Maya does not read; metallic-roughness defaults are used");
            auto converted = GltfMaterial{text(source.name), valid_material(wanted, path, document.warnings), {}};
            converted.maps[size_t(GltfMap::base_color)] = texture_use(pbr.base_color_texture, path + ".pbrMetallicRoughness.baseColorTexture");
            converted.maps[size_t(GltfMap::metallic_roughness)] =
                texture_use(pbr.metallic_roughness_texture, path + ".pbrMetallicRoughness.metallicRoughnessTexture");
            converted.maps[size_t(GltfMap::normal)] = texture_use(source.normal_texture, path + ".normalTexture");
            converted.maps[size_t(GltfMap::occlusion)] = texture_use(source.occlusion_texture, path + ".occlusionTexture");
            converted.maps[size_t(GltfMap::emissive)] = texture_use(source.emissive_texture, path + ".emissiveTexture");
            document.materials.push_back(std::move(converted));
        }
    }

    /// An attribute's accessor must have the expected number of components and as many elements as POSITION.
    const cgltf_accessor* attribute(const cgltf_primitive& primitive, cgltf_attribute_type type, std::initializer_list<size_t> widths,
                                    size_t count, const std::string& path, const char* name) {
        const auto* accessor = cgltf_find_accessor(&primitive, type, 0);
        if (!accessor) return nullptr;
        const auto width = cgltf_num_components(accessor->type);
        if (std::ranges::find(widths, width) == widths.end())
            fail(path + ".attributes." + name, "has " + std::to_string(width) + " components per element, which a " + name + " cannot");
        else if (accessor->count != count)
            fail(path + ".attributes." + name, "has " + std::to_string(accessor->count) + " elements, but POSITION has " + std::to_string(count));
        return accessor;
    }

    void meshes() {
        for (size_t m = 0; m < data.meshes_count; ++m) {
            const auto& mesh = data.meshes[m];
            auto converted = GltfMesh{text(mesh.name), {}};
            bool morphs = false;
            for (size_t p = 0; p < mesh.primitives_count; ++p) {
                const auto& primitive = mesh.primitives[p];
                const auto path = at(at("meshes", m) + ".primitives", p);
                auto out = GltfPrimitive{};
                if (primitive.material) out.material = uint32_t(cgltf_material_index(&data, primitive.material));
                morphs = morphs || primitive.targets_count > 0;
                const auto* position = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
                if (primitive.type != cgltf_primitive_type_triangles && primitive.type != cgltf_primitive_type_triangle_strip &&
                    primitive.type != cgltf_primitive_type_triangle_fan) {
                    warn(path, "draws points or lines, which Maya does not draw; it is left out");
                    out.drawable = false;
                } else if (!position) {
                    warn(path, "has no POSITION, so there is nothing to draw; it is left out");
                    out.drawable = false;
                } else {
                    out.vertices = position->count;
                    if (cgltf_num_components(position->type) != 3)
                        fail(path + ".attributes.POSITION", "must have 3 components per element");
                    attribute(primitive, cgltf_attribute_type_normal, {3}, position->count, path, "NORMAL");
                    attribute(primitive, cgltf_attribute_type_tangent, {4}, position->count, path, "TANGENT");
                    attribute(primitive, cgltf_attribute_type_texcoord, {2}, position->count, path, "TEXCOORD_0");
                    attribute(primitive, cgltf_attribute_type_color, {3, 4}, position->count, path, "COLOR_0");
                    if (primitive.indices && primitive.indices->is_sparse) fail(path + ".indices", "is sparse, which indices cannot be");
                }
                converted.primitives.push_back(out);
            }
            if (morphs) warn(at("meshes", m), "has morph targets, which are not imported; its base shape is used");
            document.meshes.push_back(std::move(converted));
        }
    }

    void cameras() {
        for (size_t i = 0; i < data.cameras_count; ++i) {
            const auto& camera = data.cameras[i];
            auto converted = GltfCamera{text(camera.name), {}};
            if (camera.type != cgltf_camera_type_perspective) {
                warn(at("cameras", i), "is orthographic, which Maya's cameras are not; it is left out");
            } else {
                const auto& perspective = camera.data.perspective;
                converted.camera.vertical_fov = perspective.yfov;
                converted.camera.near_clip = perspective.znear;
                // An infinite far plane becomes a finite one, far enough for any scene Maya draws.
                converted.camera.far_clip = perspective.has_zfar ? perspective.zfar : std::max(1000.0f, perspective.znear * 1e5f);
                auto value = ComponentValue{converted.camera};
                if (const auto valid = validate_component(value); !valid)
                    warn(at("cameras", i), std::string(valid.message) + "; the default camera is used");
                else converted.camera = std::get<CameraComponent>(value);
            }
            document.cameras.push_back(std::move(converted));
        }
    }

    void lights() {
        for (size_t i = 0; i < data.lights_count; ++i) {
            const auto& light = data.lights[i];
            auto converted = GltfLight{text(light.name), {}};
            auto& out = converted.light;
            out.color = {light.color[0], light.color[1], light.color[2]};
            if (light.type == cgltf_light_type_directional) {
                out.kind = LightKind::directional;
                out.intensity = light.intensity; // lux, as glTF's
            } else {
                // Candela, as glTF and Maya measure point and spot lights.
                out.kind = light.type == cgltf_light_type_spot ? LightKind::spot : LightKind::point;
                out.intensity = light.intensity;
                // An unbounded light reaches as far as it gives 0.01 lux (docs/import.md#lights).
                out.range = light.range > 0 ? light.range : std::clamp(std::sqrt(light.intensity / 0.01f), 0.1f, 10000.0f);
                if (out.kind == LightKind::spot) { // glTF's cone angles are measured from the axis: half Maya's
                    out.inner_cone = 2 * light.spot_inner_cone_angle;
                    out.outer_cone = 2 * light.spot_outer_cone_angle;
                }
            }
            auto value = ComponentValue{out};
            if (const auto valid = validate_component(value); !valid)
                warn(at("extensions.KHR_lights_punctual.lights", i), std::string(valid.message) + "; the default light is used");
            else out = std::get<LightComponent>(value);
            document.lights.push_back(std::move(converted));
        }
    }

    void nodes() {
        for (size_t i = 0; i < data.nodes_count; ++i) {
            const auto& node = data.nodes[i];
            auto converted = GltfNode{text(node.name), std::nullopt, {}, node_transform(node, at("nodes", i), document.warnings), {}, {}, {}};
            if (node.parent) converted.parent = uint32_t(cgltf_node_index(&data, node.parent));
            for (size_t c = 0; c < node.children_count; ++c) converted.children.push_back(uint32_t(cgltf_node_index(&data, node.children[c])));
            if (node.mesh) converted.mesh = uint32_t(cgltf_mesh_index(&data, node.mesh));
            if (node.camera && node.camera->type == cgltf_camera_type_perspective)
                converted.camera = uint32_t(cgltf_camera_index(&data, node.camera));
            if (node.light) converted.light = uint32_t(cgltf_light_index(&data, node.light));
            document.nodes.push_back(std::move(converted));
        }
        const auto* scene = data.scene ? data.scene : data.scenes_count > 0 ? &data.scenes[0] : nullptr;
        if (scene) {
            for (size_t i = 0; i < scene->nodes_count; ++i) document.roots.push_back(uint32_t(cgltf_node_index(&data, scene->nodes[i])));
        } else {
            for (size_t i = 0; i < data.nodes_count; ++i)
                if (!data.nodes[i].parent) document.roots.push_back(uint32_t(i));
        }
        if (data.skins_count > 0) warn("skins", std::to_string(data.skins_count) + " skin(s) are not imported; skinned meshes keep their bind pose");
        if (data.animations_count > 0) warn("animations", std::to_string(data.animations_count) + " animation(s) are not imported");
    }
};

std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    auto file = std::ifstream(path, std::ios::binary);
    if (!file) return std::nullopt;
    auto bytes = std::vector<std::byte>{};
    std::transform(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>(), std::back_inserter(bytes),
                   [](char c) { return std::byte(c); });
    if (file.bad()) return std::nullopt;
    return bytes;
}

std::vector<float> unpack(const cgltf_accessor* accessor) {
    auto values = std::vector<float>(accessor ? accessor->count * cgltf_num_components(accessor->type) : 0);
    if (!values.empty()) cgltf_accessor_unpack_floats(accessor, values.data(), values.size());
    return values;
}
} // namespace

GltfFile::OpenResult GltfFile::open(const std::filesystem::path& file) {
    auto result = OpenResult{};
    auto data = std::make_unique<Data>();
    data->folder = file.parent_path();
    auto options = cgltf_options{};
    const auto name = file.string();
    if (const auto parsed = cgltf_parse_file(&options, name.c_str(), &data->data); parsed != cgltf_result_success) {
        result.errors.push_back({"", std::string("cannot be read: ") + result_text(parsed)});
        return result;
    }
    auto converter = Converter{*data->data, {}, {}};
    converter.extensions(); // before the buffers: a refused extension may need data cgltf cannot read
    if (!converter.errors.empty()) {
        result.errors = std::move(converter.errors);
        return result;
    }
    if (const auto loaded = cgltf_load_buffers(&options, data->data, name.c_str()); loaded != cgltf_result_success) {
        for (size_t i = 0; i < data->data->buffers_count; ++i)
            if (!data->data->buffers[i].data)
                result.errors.push_back({at("buffers", i), "cannot be loaded from '" + text(data->data->buffers[i].uri).substr(0, 64) +
                                                               "': " + result_text(loaded)});
        if (result.errors.empty()) result.errors.push_back({"buffers", std::string("cannot be loaded: ") + result_text(loaded)});
        return result;
    }
    // Converting reads only what parsing has checked (references in range), so it goes first and its
    // problems name their paths; cgltf_validate then checks the data itself, without paths.
    converter.images();
    converter.files();
    converter.textures();
    converter.materials();
    converter.meshes();
    converter.cameras();
    converter.lights();
    converter.nodes();
    if (!converter.errors.empty()) {
        result.errors = std::move(converter.errors);
        return result;
    }
    if (const auto valid = cgltf_validate(data->data); valid != cgltf_result_success) {
        result.errors.push_back({"", std::string("is invalid: ") + result_text(valid)});
        return result;
    }
    result.file.reset(new GltfFile(std::move(data), std::move(converter.document)));
    return result;
}

GltfGeometry GltfFile::primitive(uint32_t mesh_index, uint32_t primitive_index) const {
    auto result = GltfGeometry{};
    const auto& data = *m_data->data;
    if (mesh_index >= data.meshes_count || primitive_index >= data.meshes[mesh_index].primitives_count) {
        result.error = "there is no primitive " + std::to_string(primitive_index) + " in mesh " + std::to_string(mesh_index);
        return result;
    }
    const auto path = at(at("meshes", mesh_index) + ".primitives", primitive_index);
    const auto fail = [&](std::string message) {
        result = {};
        result.error = gltf_problem_text({path, std::move(message)});
        return result;
    };
    if (!m_document.meshes[mesh_index].primitives[primitive_index].drawable) return fail("is not drawn");
    const auto& primitive = data.meshes[mesh_index].primitives[primitive_index];
    const auto positions = unpack(cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0));
    const auto normals = unpack(cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0));
    const auto tangents = unpack(cgltf_find_accessor(&primitive, cgltf_attribute_type_tangent, 0));
    const auto uvs = unpack(cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0));
    const auto* color_accessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_color, 0);
    const auto colors = unpack(color_accessor);
    const auto color_width = color_accessor ? cgltf_num_components(color_accessor->type) : 4;
    const auto count = positions.size() / 3;

    // The stored order, then whole triangles in glTF's winding.
    auto order = std::vector<size_t>{};
    if (primitive.indices) {
        order.resize(primitive.indices->count);
        for (size_t i = 0; i < order.size(); ++i) order[i] = cgltf_accessor_read_index(primitive.indices, i);
    } else {
        order.resize(count);
        for (size_t i = 0; i < count; ++i) order[i] = i;
    }
    for (const auto index : order)
        if (index >= count) return fail("has index " + std::to_string(index) + ", but only " + std::to_string(count) + " vertices");
    auto corners = std::vector<size_t>{};
    if (primitive.type == cgltf_primitive_type_triangles) {
        if (order.size() % 3 != 0) return fail("has " + std::to_string(order.size()) + " corners, which is not whole triangles");
        corners = std::move(order);
    } else if (primitive.type == cgltf_primitive_type_triangle_strip) {
        for (size_t i = 0; i + 2 < order.size(); ++i)
            corners.insert(corners.end(), {order[i], order[i + 1 + i % 2], order[i + 2 - i % 2]});
    } else {
        for (size_t i = 0; i + 2 < order.size(); ++i) corners.insert(corners.end(), {order[i + 1], order[i + 2], order[0]});
    }
    if (corners.empty()) return fail("has no triangles");
    if (corners.size() > std::numeric_limits<uint32_t>::max()) return fail("has too many corners");

    auto vertices = std::vector<Vertex>{};
    vertices.reserve(corners.size());
    for (const auto i : corners) {
        const auto position = math::Vec3{positions[3 * i], positions[3 * i + 1], positions[3 * i + 2]};
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
            return fail("attributes.POSITION: vertex " + std::to_string(i) + " is not finite");
        auto normal = normals.empty() ? math::Vec3{} : math::Vec3{normals[3 * i], normals[3 * i + 1], normals[3 * i + 2]};
        if (normal.length() > 0) normal = normal.normalized();
        auto color = math::Vec4{1.0f};
        if (!colors.empty())
            color = {colors[color_width * i], colors[color_width * i + 1], colors[color_width * i + 2], color_width == 4 ? colors[4 * i + 3] : 1.0f};
        const auto uv = uvs.empty() ? math::Vec2{0.0f} : math::Vec2{uvs[2 * i], uvs[2 * i + 1]};
        const auto tangent = tangents.empty() ? math::Vec4{1, 0, 0, 1}
                                              : math::Vec4{tangents[4 * i], tangents[4 * i + 1], tangents[4 * i + 2], tangents[4 * i + 3] < 0 ? -1.0f : 1.0f};
        vertices.emplace_back(position, normal, color, uv, tangent);
    }
    // Without normals, each triangle is flat (glTF's rule), and its tangents come from MikkTSpace, as
    // they do for any primitive without them: glTF ignores tangents given without normals.
    if (normals.empty())
        for (size_t t = 0; t < vertices.size(); t += 3) {
            auto normal = math::Vec3::cross(vertices[t + 1].position - vertices[t].position, vertices[t + 2].position - vertices[t].position);
            normal = normal.length() > 0 ? normal.normalized() : math::Vec3{0, 1, 0};
            for (size_t c = 0; c < 3; ++c) vertices[t + c].normal = normal;
        }
    if ((normals.empty() || tangents.empty()) && !generate_tangents(vertices)) return fail("generating its tangents failed");
    auto welded = weld_vertices(vertices);
    result.vertices = std::move(welded.vertices);
    result.indices = std::move(welded.indices);
    return result;
}

GltfImageBytes GltfFile::image(uint32_t index) const {
    auto result = GltfImageBytes{};
    const auto& data = *m_data->data;
    if (index >= data.images_count) {
        result.error = "there is no image " + std::to_string(index);
        return result;
    }
    const auto path = at("images", index);
    const auto fail = [&](std::string message) {
        result = {};
        result.error = gltf_problem_text({path, std::move(message)});
        return result;
    };
    const auto& image = data.images[index];
    if (image.buffer_view) {
        const auto* bytes = cgltf_buffer_view_data(image.buffer_view);
        if (!bytes) return fail("its buffer view has no data");
        const auto* begin = reinterpret_cast<const std::byte*>(bytes);
        result.bytes.assign(begin, begin + image.buffer_view->size);
        return result;
    }
    const auto uri = text(image.uri);
    if (uri.empty()) return fail("has neither a URI nor a buffer view");
    if (uri.starts_with("data:")) {
        const auto comma = uri.find(',');
        if (comma == std::string::npos || uri.rfind(";base64", comma) == std::string::npos) return fail("is a data URI that is not base64");
        const auto encoded = std::string_view(uri).substr(comma + 1);
        const auto padding = size_t(encoded.ends_with("==") ? 2 : encoded.ends_with('=') ? 1 : 0);
        const auto size = encoded.size() / 4 * 3 - padding;
        auto options = cgltf_options{};
        void* decoded = nullptr;
        if (cgltf_load_buffer_base64(&options, size, encoded.data(), &decoded) != cgltf_result_success) return fail("its data URI is not valid base64");
        const auto* begin = static_cast<const std::byte*>(decoded);
        result.bytes.assign(begin, begin + size);
        std::free(decoded);
        return result;
    }
    // A file beside the glTF file, or below its folder: never elsewhere.
    const auto& relative = m_document.images[index].uri;
    const auto source = (m_data->folder / std::filesystem::path(relative)).lexically_normal();
    const auto inside = source.lexically_relative(m_data->folder.lexically_normal());
    if (std::filesystem::path(relative).is_absolute() || relative.find("://") != std::string::npos || inside.empty() || *inside.begin() == "..")
        return fail("'" + relative + "' is outside the glTF file's folder");
    auto bytes = read_file(source);
    if (!bytes) return fail("'" + relative + "' is missing or cannot be read");
    result.bytes = std::move(*bytes);
    return result;
}
} // namespace maya
