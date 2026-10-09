// Khronos's skinning and interpolation samples, imported and posed by Maya (#1038, docs/animation.md#testing),
// against an independent evaluator: cgltf reads each file, and the glTF 2.0 specification's samplers,
// node hierarchy, and joint matrices are computed here in double precision, without Maya's reader,
// sampler, World, or renderer. At each chosen time of each clip, every node's world matrix must match its
// entity's, and every skinned instance's joint matrices must match its skin's.

#include "maya/assets/property_context.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/renderer/render_snapshot.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/rhi/null_device.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/simulation/play_session.hpp"
#include "maya/simulation/script_assets.hpp"
#include "maya/world/name_path.hpp"
#include <cgltf.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <unistd.h>
#include "support/poses.hpp"

using namespace maya;
using namespace maya::test;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

// --- The evaluator ---------------------------------------------------------------------------------

using Matrix = std::array<double, 16>; // column-major, as glTF stores matrices
Matrix identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
Matrix multiply(const Matrix& a, const Matrix& b) {
    auto m = Matrix{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            auto sum = 0.0;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
            m[c * 4 + r] = sum;
        }
    return m;
}
struct Trs {
    std::array<double, 3> t{0, 0, 0};
    std::array<double, 4> r{0, 0, 0, 1}; // x y z w
    std::array<double, 3> s{1, 1, 1};
};
/// T * R * S, the specification's local matrix.
Matrix trs_matrix(const Trs& v) {
    const auto [x, y, z, w] = v.r;
    auto m = Matrix{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
                    2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
                    2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
                    v.t[0], v.t[1], v.t[2], 1};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) m[c * 4 + r] *= v.s[size_t(c)];
    return m;
}

/// A glTF file read by cgltf alone.
double size(const Matrix& m) {
    auto largest = 1.0;
    for (const auto value : m) largest = std::max(largest, std::abs(value));
    return largest;
}
/// The tolerance for a matrix Maya computes in float through a chain of joints: relative to its size.
double tolerance(const Matrix& expected) { return 2e-5 * size(expected); }

class Evaluator {
public:
    explicit Evaluator(const fs::path& file) {
        auto options = cgltf_options{};
        REQUIRE(cgltf_parse_file(&options, file.c_str(), &m_data) == cgltf_result_success);
        REQUIRE(cgltf_load_buffers(&options, m_data, file.c_str()) == cgltf_result_success);
    }
    ~Evaluator() { cgltf_free(m_data); }
    Evaluator(const Evaluator&) = delete;
    Evaluator& operator=(const Evaluator&) = delete;

    const cgltf_data& data() const { return *m_data; }
    size_t node_index(const cgltf_node* node) const { return size_t(node - m_data->nodes); }

    /// Every node's world matrix with `animation` (or none) at `time`.
    std::vector<Matrix> world(const cgltf_animation* animation, double time) const {
        auto locals = std::vector<Trs>(m_data->nodes_count);
        auto fixed = std::vector<std::optional<Matrix>>(m_data->nodes_count); // nodes given as a matrix
        for (size_t i = 0; i < m_data->nodes_count; ++i) {
            const auto& node = m_data->nodes[i];
            if (node.has_matrix) {
                auto m = Matrix{};
                for (int k = 0; k < 16; ++k) m[size_t(k)] = node.matrix[k];
                fixed[i] = m;
            }
            if (node.has_translation) locals[i].t = {node.translation[0], node.translation[1], node.translation[2]};
            if (node.has_rotation) locals[i].r = {node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]};
            if (node.has_scale) locals[i].s = {node.scale[0], node.scale[1], node.scale[2]};
        }
        if (animation)
            for (size_t c = 0; c < animation->channels_count; ++c) {
                const auto& channel = animation->channels[c];
                if (!channel.target_node) continue;
                const auto index = node_index(channel.target_node);
                auto& local = locals[index];
                fixed[index].reset(); // an animated node is TRS (the specification forbids animating a matrix)
                switch (channel.target_path) {
                case cgltf_animation_path_type_translation: {
                    const auto v = sample(*channel.sampler, 3, time, false);
                    local.t = {v[0], v[1], v[2]};
                    break;
                }
                case cgltf_animation_path_type_rotation: {
                    const auto v = sample(*channel.sampler, 4, time, true);
                    local.r = {v[0], v[1], v[2], v[3]};
                    break;
                }
                case cgltf_animation_path_type_scale: {
                    const auto v = sample(*channel.sampler, 3, time, false);
                    local.s = {v[0], v[1], v[2]};
                    break;
                }
                default: break;
                }
            }
        auto world = std::vector<Matrix>(m_data->nodes_count, identity());
        const auto walk = [&](auto&& self, const cgltf_node* node, const Matrix& parent) -> void {
            const auto i = node_index(node);
            world[i] = multiply(parent, fixed[i] ? *fixed[i] : trs_matrix(locals[i]));
            for (size_t k = 0; k < node->children_count; ++k) self(self, node->children[k], world[i]);
        };
        const auto* scene = m_data->scene ? m_data->scene : m_data->scenes;
        for (size_t k = 0; k < scene->nodes_count; ++k) walk(walk, scene->nodes[k], identity());
        return world;
    }

    /// A skin's joint matrices: each joint's world matrix times its inverse bind matrix, with the tolerance
    /// float precision allows the product: relative to the sizes of the two it multiplies.
    std::vector<std::pair<Matrix, double>> joints(const cgltf_skin& skin, const std::vector<Matrix>& world) const {
        auto result = std::vector<std::pair<Matrix, double>>{};
        for (size_t j = 0; j < skin.joints_count; ++j) {
            auto inverse_bind = identity();
            if (skin.inverse_bind_matrices) {
                float m[16];
                REQUIRE(cgltf_accessor_read_float(skin.inverse_bind_matrices, j, m, 16));
                for (int k = 0; k < 16; ++k) inverse_bind[size_t(k)] = m[k];
            }
            const auto& joint = world[node_index(skin.joints[j])];
            result.emplace_back(multiply(joint, inverse_bind), tolerance(joint) * size(inverse_bind));
        }
        return result;
    }

private:
    /// The specification's samplers (Appendix C), keys found by a linear search.
    std::vector<double> sample(const cgltf_animation_sampler& sampler, size_t width, double time, bool rotation) const {
        const auto keys = sampler.input->count;
        const auto input = [&](size_t k) {
            float t = 0;
            REQUIRE(cgltf_accessor_read_float(sampler.input, k, &t, 1));
            return double(t);
        };
        const auto cubic = sampler.interpolation == cgltf_interpolation_type_cubic_spline;
        // A key's value; a cubic spline's element is 0 in-tangent, 1 value, 2 out-tangent.
        const auto output = [&](size_t k, size_t element) {
            float value[4] = {};
            REQUIRE(cgltf_accessor_read_float(sampler.output, cubic ? k * 3 + element : k, value, width));
            return std::vector<double>(value, value + width);
        };
        auto result = std::vector<double>{};
        if (time <= input(0)) result = output(0, 1);
        else if (time >= input(keys - 1)) result = output(keys - 1, 1);
        else {
            size_t k = 0;
            while (!(input(k) <= time && time < input(k + 1))) ++k;
            const auto t0 = input(k), t1 = input(k + 1), span = t1 - t0, s = (time - t0) / span;
            if (sampler.interpolation == cgltf_interpolation_type_step) {
                result = output(k, 1);
            } else if (cubic) {
                const auto p0 = output(k, 1), m0 = output(k, 2), p1 = output(k + 1, 1), m1 = output(k + 1, 0);
                const auto s2 = s * s, s3 = s2 * s;
                result.resize(width);
                for (size_t c = 0; c < width; ++c)
                    result[c] = (2 * s3 - 3 * s2 + 1) * p0[c] + span * (s3 - 2 * s2 + s) * m0[c] + (-2 * s3 + 3 * s2) * p1[c] +
                                span * (s3 - s2) * m1[c];
            } else if (rotation) { // spherical linear interpolation along the shorter arc
                auto a = output(k, 1), b = output(k + 1, 1);
                auto dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
                if (dot < 0) {
                    for (auto& c : b) c = -c;
                    dot = -dot;
                }
                result.resize(4);
                if (dot > 0.9995) {
                    for (size_t c = 0; c < 4; ++c) result[c] = a[c] + s * (b[c] - a[c]);
                } else {
                    const auto theta = std::acos(dot), sine = std::sin(theta);
                    const auto wa = std::sin((1 - s) * theta) / sine, wb = std::sin(s * theta) / sine;
                    for (size_t c = 0; c < 4; ++c) result[c] = wa * a[c] + wb * b[c];
                }
            } else {
                const auto a = output(k, 1), b = output(k + 1, 1);
                result.resize(width);
                for (size_t c = 0; c < width; ++c) result[c] = a[c] + s * (b[c] - a[c]);
            }
        }
        if (rotation) {
            const auto length = std::sqrt(result[0] * result[0] + result[1] * result[1] + result[2] * result[2] + result[3] * result[3]);
            for (auto& c : result) c /= length;
        }
        return result;
    }

    cgltf_data* m_data = nullptr;
};

// --- Maya's side -----------------------------------------------------------------------------------

/// The samples, imported into one temporary project.
struct SampleProject {
    fs::path root;
    Project project;
    SampleProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / ("maya-animation-samples-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(root);
        std::ofstream(root / "project.maya") << "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\n";
        std::ofstream(root / "catalog.maya") << "maya-assets 1\n";
        auto opened = open_project(root);
        REQUIRE(opened);
        project = opened.project;
    }
    ~SampleProject() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    /// Copies the file (and what it names) into the project, again when it is there, and imports it.
    GltfImportResult import(const fs::path& source) {
        const auto folder = root / "models" / source.stem();
        fs::create_directories(folder);
        fs::copy_file(source, folder / source.filename(), fs::copy_options::overwrite_existing);
        const auto opened = GltfFile::open(source);
        REQUIRE(opened);
        for (const auto& named : opened.file->document().files) {
            fs::create_directories((folder / named).parent_path());
            fs::copy_file(source.parent_path() / named, folder / named, fs::copy_options::overwrite_existing);
        }
        auto imported = import_gltf(project, folder / source.filename());
        REQUIRE(imported);
        return imported;
    }
};

double largest_difference(const Matrix& expected, const math::Mat4& actual) {
    auto worst = 0.0;
    for (int k = 0; k < 16; ++k) worst = std::max(worst, std::abs(expected[size_t(k)] - double(actual.elements[k])));
    return worst;
}

constexpr const char* samples[] = {
    "SimpleSkin/glTF/SimpleSkin.gltf",          "RiggedSimple/glTF/RiggedSimple.gltf",   "RiggedFigure/glTF/RiggedFigure.gltf",
    "CesiumMan/glTF/CesiumMan.gltf",            "Fox/glTF/Fox.gltf",                     "RecursiveSkeletons/glTF/RecursiveSkeletons.gltf",
    "InterpolationTest/glTF/InterpolationTest.gltf",
};
} // namespace

TEST_CASE("Khronos's skinning samples pose every node and joint as an independent evaluator does", "[animation][samples]") {
    if (!fs::is_directory(fs::path(MAYA_RENDER_SAMPLES) / "Models/RecursiveSkeletons")) SKIP("no samples; run tools/fetch_render_samples.sh");
    auto project = SampleProject{};
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        auto assets = std::optional<ProjectAssetsResult>{};
        for (const auto* relative : samples) {
            INFO(relative);
            const auto source = fs::path(MAYA_RENDER_SAMPLES) / "Models" / relative;
            const auto imported = project.import(source);
            assets.reset();
            auto opened = open_project_assets(project.project, std::make_unique<FileAssetProvider>(device));
            REQUIRE(opened);
            assets.emplace(std::move(opened));
            auto& registry = *assets->registry;
            const auto context = asset_property_context(registry);
            const auto evaluator = Evaluator(source);
            const auto& data = evaluator.data();
            const auto document = GltfFile::open(source);
            REQUIRE(document);
            const auto paths = gltf_node_paths(document.file->document());
            auto clips = std::vector<AssetId>(data.animations_count);
            for (const auto& record : imported.records)
                if (record.kind == AssetKind::animation) {
                    const auto text = record.path.generic_string();
                    clips.at(std::stoul(text.substr(text.rfind('/') + 1))) = record.id;
                }
            REQUIRE(data.animations_count > 0);
            size_t compared_nodes = 0, compared_joints = 0;
            for (size_t a = 0; a < data.animations_count; ++a) {
                // Each clip's duration, as the specification has it: its latest key.
                auto duration = 0.0;
                for (size_t s = 0; s < data.animations[a].samplers_count; ++s) {
                    const auto* input = data.animations[a].samplers[s].input;
                    if (input->has_max) duration = std::max(duration, double(input->max[0]));
                }
                for (const auto fraction : {0.0, 0.13, 0.37, 0.5, 0.81, 1.0}) {
                    const auto time = duration * fraction;
                    INFO("clip " << a << " at " << time << " s");
                    auto loaded = load_scene_file(project.project.content_root / imported.scene, context);
                    REQUIRE(loaded);
                    // The clip held at `time`, once and not looped, so its end is its last key.
                    auto& root = loaded.document.entities.front();
                    auto component = std::ranges::find_if(root.components, [](const auto& c) { return std::holds_alternative<AnimationComponent>(c); });
                    REQUIRE(component != root.components.end());
                    *component = AnimationComponent{{clips[a]}, false, false, 1.0f, float(time)};
                    const auto root_id = root.id;
                    auto started = PlaySession::start(std::move(loaded.document), context,
                                                      play_systems(registry_script_sources(registry), registry_animation_clips(registry)));
                    INFO(started.error);
                    REQUIRE(started);
                    auto& session = *started.session;
                    session.clock().pause();
                    session.clock().step();
                    const auto frame = session.update(0.0);
                    REQUIRE(frame.ticks_run == 1);
                    for (const auto& message : frame.messages) UNSCOPED_INFO(message.text);
                    CHECK(frame.messages.empty());
                    const auto& world = session.world();
                    const auto expected = evaluator.world(&data.animations[a], time);
                    // Every node's entity, found by its path below the import's root.
                    const auto entities = resolve_name_paths(world, *world.find(root_id), paths);
                    for (size_t n = 0; n < data.nodes_count; ++n) {
                        INFO("node " << n << " " << paths[n]);
                        REQUIRE(entities[n]);
                        const auto actual = world.world_matrix(*entities[n]);
                        REQUIRE(actual);
                        const auto difference = largest_difference(expected[n], actual->matrix());
                        INFO("largest difference " << difference);
                        CHECK(difference <= tolerance(expected[n]));
                        ++compared_nodes;
                    }
                    // Every skinned instance's joints are one of the file's skins'.
                    const auto snapshot = extract_render_snapshot(world, registry);
                    for (const auto& problem : snapshot.diagnostics) UNSCOPED_INFO(problem.message);
                    CHECK(snapshot.diagnostics.empty());
                    auto skins = std::vector<std::vector<std::pair<Matrix, double>>>{};
                    for (size_t s = 0; s < data.skins_count; ++s) skins.push_back(evaluator.joints(data.skins[s], expected));
                    auto skinned = size_t{0};
                    for (const auto& instance : snapshot.instances) {
                        if (instance.joint_count == 0) continue;
                        ++skinned;
                        auto best = std::numeric_limits<double>::infinity(); // the closest skin's worst joint, against its tolerance
                        auto matched = false;
                        for (const auto& skin : skins) {
                            if (skin.size() != instance.joint_count) continue;
                            auto worst = 0.0;
                            for (size_t j = 0; j < skin.size(); ++j)
                                worst = std::max(worst, largest_difference(skin[j].first, snapshot.joints[instance.first_joint + j]) / skin[j].second);
                            best = std::min(best, worst);
                            matched |= worst <= 1.0;
                        }
                        INFO("closest skin's worst joint: " << best << " times its tolerance");
                        CHECK(matched);
                        compared_joints += instance.joint_count;
                    }
                    // Each skinned node's primitives are drawn skinned.
                    auto expected_skinned = size_t{0};
                    for (size_t n = 0; n < data.nodes_count; ++n)
                        if (data.nodes[n].skin && data.nodes[n].mesh) expected_skinned += data.nodes[n].mesh->primitives_count;
                    CHECK(skinned == expected_skinned);
                }
            }
            CHECK(compared_nodes > 0);
            if (data.skins_count > 0) CHECK(compared_joints > 0);
        }
    }
    device.shutdown();
}

TEST_CASE("Imports catalog skins and clips as parts of the file, skin their meshes, and play the first clip", "[animation][samples][import]") {
    if (!fs::is_directory(fs::path(MAYA_RENDER_SAMPLES) / "Models/Fox")) SKIP("no samples; run tools/fetch_render_samples.sh");
    auto project = SampleProject{};
    const auto source = fs::path(MAYA_RENDER_SAMPLES) / "Models/Fox/glTF-Binary/Fox.glb";
    const auto first = project.import(source);
    const auto parts = [](const GltfImportResult& result, AssetKind kind) {
        auto found = std::map<std::string, AssetId>{};
        for (const auto& record : result.records)
            if (record.kind == kind) found.emplace(record.path.generic_string(), record.id);
        return found;
    };
    const auto skins = parts(first, AssetKind::skin), clips = parts(first, AssetKind::animation);
    CHECK(skins == std::map<std::string, AssetId>{{"models/Fox/Fox.glb#skin/0", skins.begin()->second}});
    REQUIRE(clips.size() == 3);
    CHECK(clips.contains("models/Fox/Fox.glb#animation/0"));
    CHECK(clips.contains("models/Fox/Fox.glb#animation/2"));
    // The import file names them by the file's names, so reimporting keeps their IDs.
    const auto import_text = [&] {
        auto input = std::ifstream(project.root / "models/Fox/Fox.glb.import");
        return std::string(std::istreambuf_iterator<char>(input), {});
    }();
    CHECK(import_text.find("\"animation/1\" \"Walk\"") != std::string::npos);
    CHECK(import_text.find("\"skin/0\" \"#0\"") != std::string::npos); // the Fox's skin has no name
    const auto again = project.import(source);
    CHECK(parts(again, AssetKind::skin) == skins);
    CHECK(parts(again, AssetKind::animation) == clips);
    // The scene: the root plays the first clip (Survey), and the skinned mesh names the skin.
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    {
        auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(device));
        REQUIRE(assets);
        const auto loaded = load_scene_file(project.root / first.scene, asset_property_context(*assets.registry));
        REQUIRE(loaded);
        const auto& root = loaded.document.entities.front();
        CHECK_FALSE(root.parent);
        const auto animation = std::ranges::find_if(root.components, [](const auto& c) { return std::holds_alternative<AnimationComponent>(c); });
        REQUIRE(animation != root.components.end());
        CHECK(std::get<AnimationComponent>(*animation).clip.id == clips.at("models/Fox/Fox.glb#animation/0"));
        CHECK(std::get<AnimationComponent>(*animation).playing);
        CHECK(std::get<AnimationComponent>(*animation).loop);
        auto skinned = 0;
        for (const auto& entity : loaded.document.entities)
            for (const auto& component : entity.components)
                if (const auto* skin = std::get_if<SkinComponent>(&component)) {
                    CHECK(skin->skin.id == skins.begin()->second);
                    ++skinned;
                }
        CHECK(skinned == 1);
        // The skin and clips load, and Survey's channels are the file's.
        const auto skin = assets.registry->acquire(AssetRef<SkinAsset>{skins.begin()->second});
        REQUIRE(skin);
        CHECK(skin.lease.value().joints.size() == 24);
        CHECK(skin.lease.value().joints.front() == "root/_rootJoint"); // the file's first joint, by its path below the root
        const auto survey = assets.registry->acquire(AssetRef<AnimationAsset>{clips.at("models/Fox/Fox.glb#animation/0")});
        REQUIRE(survey);
        CHECK(survey.lease.value().name == "Survey");
        CHECK(survey.lease.value().duration == Approx(3.4166667f));
        CHECK_FALSE(survey.lease.value().channels.empty());
    }
    device.shutdown();
}

namespace {
/// Records which vertex entry each pipeline has, what is drawn with each, and the joint palette's binding.
class SkinningDevice final : public NullGraphicsDevice {
public:
    SkinningDevice() { REQUIRE(initialize(nullptr, {3, size_t{16} << 20})); }
    ~SkinningDevice() override { shutdown(); }
    std::map<uint32_t, std::string> entries; // by pipeline slot
    std::map<std::string, size_t> draws; // by vertex entry
    size_t palettes = 0; // binds of buffer 6

protected:
    RhiDiagnostic backend_create_pipeline(uint32_t slot, const PipelineDesc& desc) override {
        entries[slot] = desc.vertex_entry;
        return NullGraphicsDevice::backend_create_pipeline(slot, desc);
    }
    void backend_set_pipeline(uint32_t slot) override { m_bound = slot; }
    void backend_set_uniform_buffer(uint32_t index, uint32_t, size_t) override { palettes += index == 6; }
    void backend_draw_indexed(uint32_t, IndexType, uint32_t, size_t, uint32_t, uint32_t) override { ++draws[entries[m_bound]]; }

private:
    uint32_t m_bound = 0;
};
} // namespace

TEST_CASE("Skinned meshes draw with the skinned pipelines, bounded where their joints take them, and unbound skins are reported",
          "[animation][samples][renderer]") {
    const auto source = fs::path(MAYA_RENDER_SAMPLES) / "Models/CesiumMan/glTF-Binary/CesiumMan.glb";
    if (!fs::exists(source)) SKIP("no samples; run tools/fetch_render_samples.sh");
    auto project = SampleProject{};
    const auto imported = project.import(source);
    const auto file = GltfFile::open(source);
    REQUIRE(file);
    const auto geometry = file.file->primitive(0, 0);
    REQUIRE(geometry);
    REQUIRE(geometry.skin.size() == geometry.vertices.size());
    SkinningDevice device;
    {
        auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(device));
        REQUIRE(assets);
        auto& registry = *assets.registry;
        const auto context = asset_property_context(registry);
        auto loaded = load_scene_file(project.project.content_root / imported.scene, context);
        REQUIRE(loaded);
        auto started = PlaySession::start(std::move(loaded.document), context,
                                          play_systems(registry_script_sources(registry), registry_animation_clips(registry)));
        REQUIRE(started);
        auto& session = *started.session;
        auto renderer = Renderer(device, "test source");
        auto target = RenderTarget(device);
        REQUIRE_FALSE(target.resize(64, 64));
        auto sun = LightComponent{};
        sun.cast_shadows = true;
        auto commands = session.world().commands();
        const auto light = commands.create();
        commands.add(light, TransformComponent{{0, 5, 5}, math::Quat::from_axis_angle({1, 0, 0}, -0.8f), {1, 1, 1}});
        commands.add(light, sun);
        REQUIRE(session.world().commit(commands));
        auto lens = CameraComponent{};
        const auto view = make_render_view(lens, math::Mat4::translate({0, 0.8f, 4}), 64, 64);
        REQUIRE(view);
        auto cache = SkinBindingCache{};
        auto cached = RenderExtractOptions{};
        cached.skins = &cache;
        for (int second = 0; second < 2; ++second) {
            session.update(0.37); // a walking pose
            if (second == 1) { // the spine carried 5 m away, with the mesh entity left where it was: the bounds follow the joints
                auto move = session.world().commands();
                session.world().for_each<NameComponent, TransformComponent>(
                    [&](EntityHandle entity, const NameComponent& name, const TransformComponent& transform) {
                        if (name.value != "Skeleton_torso_joint_1") return;
                        auto moved = transform;
                        moved.translation = moved.translation + math::Vec3{5, 0, 0};
                        move.set_transform(entity, moved);
                    });
                REQUIRE(session.world().commit(move));
            }
            const auto snapshot = extract_render_snapshot(session.world(), registry, cached);
            REQUIRE(snapshot.diagnostics.empty());
            // The cache keeps the joints found between extractions, and changes nothing they give.
            CHECK(cache.bindings.size() == 1);
            const auto searched = extract_render_snapshot(session.world(), registry);
            REQUIRE(searched.joints.size() == snapshot.joints.size());
            for (size_t j = 0; j < snapshot.joints.size(); ++j)
                CHECK(std::memcmp(searched.joints[j].elements, snapshot.joints[j].elements, sizeof(math::Mat4)) == 0);
            REQUIRE(snapshot.instances.size() == 1);
            const auto& instance = snapshot.instances.front();
            REQUIRE(instance.joint_count == 19);
            // Every vertex, skinned on the CPU with the snapshot's joints, is inside the instance's bounds.
            auto outside = 0.0f;
            for (size_t v = 0; v < geometry.vertices.size(); ++v) {
                const auto& skin = geometry.skin[v];
                const auto p = geometry.vertices[v].position;
                auto placed = math::Vec3{0.0f};
                for (size_t k = 0; k < 4; ++k) {
                    const auto& m = snapshot.joints[instance.first_joint + skin.joints[k]];
                    placed = placed + math::Vec3{m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
                                                 m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
                                                 m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)} * skin.weights[k];
                }
                outside = std::max(outside, (placed - instance.bounds_center).length() - instance.bounds_radius);
            }
            CHECK(outside <= 1e-4f);
            CHECK(std::isfinite(instance.bounds_radius));
            CHECK(instance.bounds_radius < 3.0f); // around a person 1.5 m tall: loose, since each joint carries the whole mesh's bounds
            REQUIRE_FALSE(device.begin_frame());
            REQUIRE_FALSE(renderer.render(snapshot, *view, target));
            REQUIRE_FALSE(device.end_frame());
            CHECK(renderer.stats().joints == 19u * (second + 1));
        }
        // The lit pass and the sun's cascades draw it skinned, with the palette bound for each pass.
        CHECK(device.draws["litSkinnedVertex"] == 2);
        CHECK(device.draws["shadowSkinnedVertex"] >= 2);
        CHECK(device.draws["litVertex"] == 0);
        CHECK(device.draws["shadowVertex"] == 0);
        CHECK(device.palettes >= 4);
        // A joint renamed: the mesh is drawn as authored, and that is reported.
        auto rename = session.world().commands();
        auto renamed = false;
        session.world().for_each<NameComponent>([&](EntityHandle entity, const NameComponent& name) {
            if (name.value == "Skeleton_torso_joint_2") {
                rename.replace(entity, NameComponent{"Spine"});
                renamed = true;
            }
        });
        REQUIRE(renamed);
        REQUIRE(session.world().commit(rename));
        const auto unbound = extract_render_snapshot(session.world(), registry, cached); // the cache sees the rename
        REQUIRE(unbound.instances.size() == 1);
        CHECK(unbound.instances.front().joint_count == 0);
        CHECK(std::isfinite(unbound.instances.front().bounds_radius));
        REQUIRE(unbound.diagnostics.size() == 1);
        CHECK(unbound.diagnostics[0].code == RenderIssue::unbound_skin);
        CHECK(unbound.diagnostics[0].message.find("Skeleton_torso_joint_2' is not below it or its ancestors (renamed or removed?)") !=
              std::string::npos);
    }
}
