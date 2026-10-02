// glTF 2.0 parser comparison for #1030: cgltf, fastgltf, and tinygltf 3 load each sample model (parse,
// external buffers, and validation), repeatedly, and then five broken files, to compare speed, what
// they report, and what they accept. Usage: maya_import_prototype <render-samples folder>
#include <cgltf.h>
#include <fastgltf/core.hpp>
#include <tiny_gltf_v3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Outcome {
    bool ok = false;
    std::string error;
    size_t meshes = 0, primitives = 0, materials = 0, textures = 0, images = 0, nodes = 0, skins = 0, animations = 0;
    size_t buffer_bytes = 0;
    std::vector<std::string> required;
};

const char* cgltf_error(cgltf_result result) {
    switch (result) {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "legacy glTF";
    default: return "unknown error";
    }
}

Outcome with_cgltf(const fs::path& file) {
    auto outcome = Outcome{};
    auto options = cgltf_options{};
    cgltf_data* data = nullptr;
    auto result = cgltf_parse_file(&options, file.c_str(), &data);
    if (result == cgltf_result_success) result = cgltf_load_buffers(&options, data, file.c_str());
    if (result == cgltf_result_success) result = cgltf_validate(data);
    if (result != cgltf_result_success) {
        outcome.error = cgltf_error(result);
        cgltf_free(data);
        return outcome;
    }
    outcome.ok = true;
    outcome.meshes = data->meshes_count;
    for (size_t i = 0; i < data->meshes_count; ++i) outcome.primitives += data->meshes[i].primitives_count;
    outcome.materials = data->materials_count;
    outcome.textures = data->textures_count;
    outcome.images = data->images_count;
    outcome.nodes = data->nodes_count;
    outcome.skins = data->skins_count;
    outcome.animations = data->animations_count;
    for (size_t i = 0; i < data->buffers_count; ++i) outcome.buffer_bytes += data->buffers[i].size;
    for (size_t i = 0; i < data->extensions_required_count; ++i) outcome.required.emplace_back(data->extensions_required[i]);
    cgltf_free(data);
    return outcome;
}

constexpr auto every_extension = [] {
    using fastgltf::Extensions;
    auto all = Extensions::None;
    for (const auto e : {Extensions::KHR_texture_transform, Extensions::KHR_texture_basisu, Extensions::MSFT_texture_dds,
                         Extensions::KHR_mesh_quantization, Extensions::EXT_meshopt_compression, Extensions::KHR_lights_punctual,
                         Extensions::EXT_texture_webp, Extensions::KHR_materials_specular, Extensions::KHR_materials_ior,
                         Extensions::KHR_materials_iridescence, Extensions::KHR_materials_volume, Extensions::KHR_materials_transmission,
                         Extensions::KHR_materials_clearcoat, Extensions::KHR_materials_emissive_strength, Extensions::KHR_materials_sheen,
                         Extensions::KHR_materials_unlit, Extensions::KHR_materials_anisotropy, Extensions::EXT_mesh_gpu_instancing,
                         Extensions::KHR_materials_pbrSpecularGlossiness, Extensions::KHR_materials_variants,
                         Extensions::KHR_draco_mesh_compression, Extensions::KHR_materials_dispersion})
        all = all | e;
    return all;
}();

Outcome with_fastgltf(const fs::path& file) {
    auto outcome = Outcome{};
    auto parser = fastgltf::Parser(every_extension);
    auto data = fastgltf::GltfDataBuffer::FromPath(file);
    if (data.error() != fastgltf::Error::None) {
        outcome.error = std::string(fastgltf::getErrorMessage(data.error()));
        return outcome;
    }
    auto asset = parser.loadGltf(data.get(), file.parent_path(), fastgltf::Options::LoadExternalBuffers);
    if (asset.error() != fastgltf::Error::None) {
        outcome.error = std::string(fastgltf::getErrorName(asset.error())) + ": " + std::string(fastgltf::getErrorMessage(asset.error()));
        return outcome;
    }
    if (const auto invalid = fastgltf::validate(asset.get()); invalid != fastgltf::Error::None) {
        outcome.error = "validate: " + std::string(fastgltf::getErrorMessage(invalid));
        return outcome;
    }
    const auto& a = asset.get();
    outcome.ok = true;
    outcome.meshes = a.meshes.size();
    for (const auto& mesh : a.meshes) outcome.primitives += mesh.primitives.size();
    outcome.materials = a.materials.size();
    outcome.textures = a.textures.size();
    outcome.images = a.images.size();
    outcome.nodes = a.nodes.size();
    outcome.skins = a.skins.size();
    outcome.animations = a.animations.size();
    for (const auto& buffer : a.buffers) outcome.buffer_bytes += buffer.byteLength;
    for (const auto& name : a.extensionsRequired) outcome.required.emplace_back(name);
    return outcome;
}

Outcome with_tinygltf(const fs::path& file) {
    auto outcome = Outcome{};
    auto options = tg3_parse_options{};
    tg3_parse_options_init(&options);
    options.images_as_is = 1; // images are decoded separately, as for the other parsers
    auto errors = tg3_error_stack{};
    tg3_error_stack_init(&errors);
    auto model = tg3_model{};
    const auto name = file.string();
    const auto result = tg3_parse_file(&model, &errors, name.c_str(), uint32_t(name.size()), &options);
    if (result != TG3_OK || errors.has_error) {
        for (uint32_t i = 0; i < errors.count; ++i) {
            if (!outcome.error.empty()) outcome.error += "; ";
            outcome.error += errors.entries[i].message ? errors.entries[i].message : "?";
            if (errors.entries[i].json_path) outcome.error += std::string(" at ") + errors.entries[i].json_path;
        }
        if (outcome.error.empty()) outcome.error = "error code " + std::to_string(int(result));
    } else {
        outcome.ok = true;
        outcome.meshes = model.meshes_count;
        for (uint32_t i = 0; i < model.meshes_count; ++i) outcome.primitives += model.meshes[i].primitives_count;
        outcome.materials = model.materials_count;
        outcome.textures = model.textures_count;
        outcome.images = model.images_count;
        outcome.nodes = model.nodes_count;
        outcome.skins = model.skins_count;
        outcome.animations = model.animations_count;
        for (uint32_t i = 0; i < model.buffers_count; ++i) outcome.buffer_bytes += model.buffers[i].data.count;
        for (uint32_t i = 0; i < model.extensions_required_count; ++i)
            outcome.required.emplace_back(model.extensions_required[i].data, model.extensions_required[i].len);
    }
    tg3_model_free(&model);
    tg3_error_stack_free(&errors);
    return outcome;
}

struct Parser {
    const char* name;
    std::function<Outcome(const fs::path&)> load;
};

double median_ms(const std::function<Outcome(const fs::path&)>& load, const fs::path& file, int runs) {
    auto times = std::vector<double>{};
    for (int i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        load(file);
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::ranges::sort(times);
    return times[times.size() / 2];
}

std::string read(const fs::path& file) {
    auto in = std::ifstream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path& file, const std::string& text) { std::ofstream(file, std::ios::binary) << text; }
std::string replace_once(std::string text, const std::string& from, const std::string& to) {
    if (const auto at = text.find(from); at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

/// Animation: samples every channel of a clip at a time (linear and step keyframes; rotations are
/// normalized-lerped), composes the joints' world matrices, and returns the skin's joint palette.
struct Pose {
    std::vector<std::array<float, 16>> palette;
};
void sample_clip(cgltf_data* data, const cgltf_animation& clip, float time) {
    for (size_t c = 0; c < clip.channels_count; ++c) {
        const auto& channel = clip.channels[c];
        const auto* sampler = channel.sampler;
        const auto keys = sampler->input->count;
        auto lo = size_t{0};
        float t0 = 0, t1 = 0;
        for (size_t k = 0; k + 1 < keys; ++k) {
            cgltf_accessor_read_float(sampler->input, k + 1, &t1, 1);
            if (t1 >= time) {
                lo = k;
                break;
            }
            lo = k + 1;
        }
        cgltf_accessor_read_float(sampler->input, lo, &t0, 1);
        const auto hi = std::min(lo + 1, keys - 1);
        cgltf_accessor_read_float(sampler->input, hi, &t1, 1);
        const auto f = t1 > t0 ? std::clamp((time - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
        float a[4] = {}, b[4] = {}, v[4] = {};
        const auto width = channel.target_path == cgltf_animation_path_type_rotation ? 4 : 3;
        cgltf_accessor_read_float(sampler->output, lo, a, size_t(width));
        cgltf_accessor_read_float(sampler->output, hi, b, size_t(width));
        for (int i = 0; i < width; ++i) v[i] = sampler->interpolation == cgltf_interpolation_type_step ? a[i] : a[i] + (b[i] - a[i]) * f;
        auto* node = channel.target_node;
        if (channel.target_path == cgltf_animation_path_type_translation) std::copy(v, v + 3, node->translation);
        else if (channel.target_path == cgltf_animation_path_type_scale) std::copy(v, v + 3, node->scale);
        else if (channel.target_path == cgltf_animation_path_type_rotation) {
            const auto l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
            for (int i = 0; i < 4; ++i) node->rotation[i] = v[i] / l;
        }
    }
    (void)data;
}
void mul(const float* a, const float* b, float* out) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            auto s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}
Pose joint_palette(const cgltf_skin& skin) {
    auto pose = Pose{};
    pose.palette.resize(skin.joints_count);
    for (size_t j = 0; j < skin.joints_count; ++j) {
        float world[16], inverse_bind[16];
        cgltf_node_transform_world(skin.joints[j], world);
        cgltf_accessor_read_float(skin.inverse_bind_matrices, j, inverse_bind, 16);
        mul(world, inverse_bind, pose.palette[j].data());
    }
    return pose;
}
/// Skins every vertex of the skin's mesh on the CPU (four weights each), for comparison.
size_t skin_vertices(const cgltf_primitive& primitive, const Pose& pose, std::vector<float>& out) {
    const cgltf_accessor *positions = nullptr, *joints = nullptr, *weights = nullptr;
    for (size_t a = 0; a < primitive.attributes_count; ++a) {
        const auto& attribute = primitive.attributes[a];
        if (attribute.type == cgltf_attribute_type_position) positions = attribute.data;
        if (attribute.type == cgltf_attribute_type_joints && attribute.index == 0) joints = attribute.data;
        if (attribute.type == cgltf_attribute_type_weights && attribute.index == 0) weights = attribute.data;
    }
    if (!positions || !joints || !weights) return 0;
    out.resize(positions->count * 3);
    for (size_t v = 0; v < positions->count; ++v) {
        float p[3], w[4];
        cgltf_uint j[4];
        cgltf_accessor_read_float(positions, v, p, 3);
        cgltf_accessor_read_uint(joints, v, j, 4);
        cgltf_accessor_read_float(weights, v, w, 4);
        float r[3] = {0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            const auto& m = pose.palette[std::min<size_t>(j[k], pose.palette.size() - 1)];
            for (int i = 0; i < 3; ++i) r[i] += w[k] * (m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i]);
        }
        std::copy(r, r + 3, &out[v * 3]);
    }
    return positions->count;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: maya_import_prototype <render-samples folder>\n");
        return 2;
    }
    if (!fs::exists(fs::path(argv[1]) / "Models")) {
        std::printf("No sample content in %s; run tools/fetch_render_samples.sh\n", argv[1]);
        return 77; // CTest's skip code for these prototypes
    }
    const auto models = fs::path(argv[1]) / "Models";
    const Parser parsers[] = {{"cgltf", with_cgltf}, {"fastgltf", with_fastgltf}, {"tinygltf3", with_tinygltf}};
    const char* files[] = {"BoxTextured/glTF/BoxTextured.gltf", "BoxTextured/glTF-Binary/BoxTextured.glb",
                           "DamagedHelmet/glTF-Binary/DamagedHelmet.glb", "FlightHelmet/glTF/FlightHelmet.gltf",
                           "CesiumMan/glTF-Binary/CesiumMan.glb", "Fox/glTF-Binary/Fox.glb",
                           "MetalRoughSpheres/glTF-Binary/MetalRoughSpheres.glb", "TextureTransformTest/glTF/TextureTransformTest.gltf",
                           "NormalTangentMirrorTest/glTF-Binary/NormalTangentMirrorTest.glb", "Sponza/glTF/Sponza.gltf",
                           "ABeautifulGame/glTF/ABeautifulGame.gltf", "ABeautifulGame/glTF-Binary/ABeautifulGame.glb",
                           "FlightHelmet/glTF-KTX-BasisU/FlightHelmet.gltf"};

    // What the decision relies on fails the run (CTest's `prototype` label); the rest is reported.
    auto failures = 0;
    const auto unexpected = [&](const std::string& what) {
        std::printf("UNEXPECTED: %s\n", what.c_str());
        ++failures;
    };

    std::printf("Loading (parse, external buffers, validation); median of 15 runs, warm file cache\n\n");
    std::printf("%-58s %-10s %9s  %s\n", "file", "parser", "ms", "meshes/prims/materials/textures/images/nodes/skins/anims, buffer MiB");
    for (const auto* relative : files) {
        const auto file = models / relative;
        if (!fs::exists(file)) {
            unexpected(std::string(relative) + " is missing; run tools/fetch_render_samples.sh");
            continue;
        }
        for (const auto& parser : parsers) {
            const auto outcome = parser.load(file);
            if (!outcome.ok) {
                std::printf("%-58s %-10s   refused  %s\n", relative, parser.name, outcome.error.c_str());
                unexpected(std::string(parser.name) + " refused a valid sample");
                continue;
            }
            const auto ms = median_ms(parser.load, file, 15);
            std::printf("%-58s %-10s %9.3f  %zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu, %.1f", relative, parser.name, ms, outcome.meshes,
                        outcome.primitives, outcome.materials, outcome.textures, outcome.images, outcome.nodes, outcome.skins,
                        outcome.animations, double(outcome.buffer_bytes) / (1 << 20));
            if (!outcome.required.empty()) {
                std::printf("; requires");
                for (const auto& name : outcome.required) std::printf(" %s", name.c_str());
            }
            std::printf("\n");
        }
    }

    if (failures > 0) return 1; // the cases below are made from the samples

    // Broken files, made from the samples in a scratch folder.
    const auto scratch = fs::temp_directory_path() / "maya-import-prototype";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const auto box = read(models / "BoxTextured/glTF/BoxTextured.gltf");
    const auto glb = read(models / "DamagedHelmet/glTF-Binary/DamagedHelmet.glb");
    fs::copy_file(models / "BoxTextured/glTF/BoxTextured0.bin", scratch / "BoxTextured0.bin");
    write(scratch / "truncated.glb", glb.substr(0, glb.size() * 3 / 5));
    write(scratch / "syntax.gltf", box.substr(0, box.size() - 3));
    write(scratch / "index.gltf", replace_once(box, "\"indices\": 0", "\"indices\": 99"));
    write(scratch / "version.gltf", replace_once(box, "\"version\": \"2.0\"", "\"version\": \"1.0\""));
    write(scratch / "missing_buffer.gltf", replace_once(box, "BoxTextured0.bin", "absent.bin"));
    std::printf("\nBroken files\n\n");
    for (const auto* broken : {"truncated.glb", "syntax.gltf", "index.gltf", "version.gltf", "missing_buffer.gltf"})
        for (const auto& parser : parsers) {
            const auto outcome = parser.load(scratch / broken);
            std::printf("%-22s %-10s %s\n", broken, parser.name, outcome.ok ? "ACCEPTED" : ("refused: " + outcome.error).c_str());
            if (outcome.ok && std::string_view(parser.name) == "cgltf") unexpected(std::string("cgltf accepted ") + broken);
        }
    fs::remove_all(scratch);

    // Animation cost on the CPU: sampling a clip and building the joint palette (what the fixed tick
    // would do), and skinning the vertices on the CPU (which GPU skinning would replace).
    std::printf("\nAnimation (cgltf; mean of 1,000 evaluations at spread-out times)\n\n");
    for (const auto* relative : {"CesiumMan/glTF-Binary/CesiumMan.glb", "Fox/glTF-Binary/Fox.glb"}) {
        auto options = cgltf_options{};
        cgltf_data* data = nullptr;
        const auto file = (models / relative).string();
        if (cgltf_parse_file(&options, file.c_str(), &data) != cgltf_result_success ||
            cgltf_load_buffers(&options, data, file.c_str()) != cgltf_result_success || data->skins_count == 0 || data->animations_count == 0) {
            unexpected(std::string(relative) + " cannot be evaluated");
            cgltf_free(data);
            continue;
        }
        const auto& clip = data->animations[0];
        auto duration = 0.0f;
        for (size_t c = 0; c < clip.channels_count; ++c) {
            float last = 0;
            cgltf_accessor_read_float(clip.channels[c].sampler->input, clip.channels[c].sampler->input->count - 1, &last, 1);
            duration = std::max(duration, last);
        }
        const cgltf_primitive* skinned = nullptr;
        for (size_t n = 0; n < data->nodes_count && !skinned; ++n)
            if (data->nodes[n].skin && data->nodes[n].mesh) skinned = &data->nodes[n].mesh->primitives[0];
        constexpr int runs = 1000;
        auto sample_ms = 0.0, palette_ms = 0.0, skin_ms = 0.0;
        auto out = std::vector<float>{};
        auto vertices = size_t{0};
        for (int i = 0; i < runs; ++i) {
            const auto time = duration * float(i) / runs;
            auto start = std::chrono::steady_clock::now();
            sample_clip(data, clip, time);
            sample_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            start = std::chrono::steady_clock::now();
            const auto pose = joint_palette(data->skins[0]);
            palette_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            start = std::chrono::steady_clock::now();
            if (skinned) vertices = skin_vertices(*skinned, pose, out);
            skin_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
        std::printf("%-38s %zu joints, %zu channels, %zu vertices: sample %.4f ms, palette %.4f ms, CPU skinning %.4f ms\n", relative,
                    data->skins[0].joints_count, clip.channels_count, vertices, sample_ms / runs, palette_ms / runs, skin_ms / runs);
        if (vertices == 0) unexpected(std::string(relative) + " has no skinned vertices");
        cgltf_free(data);
    }
    return failures == 0 ? 0 : 1;
}
