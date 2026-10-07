#include "r1.hpp"
#include "maya/assets/gltf.hpp"
#include "maya/assets/import_file.hpp"
#include "maya/assets/material_file.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/registry.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/world/spatial.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace maya::r1 {
namespace fs = std::filesystem;
namespace {

// Fixed IDs for what the assembler authors ("R1" and an index), so scenes and tests name them.
constexpr uint64_t r1_id = 0x52310000;
constexpr AssetId environment_id{r1_id, 1}, floor_mesh_id{r1_id, 2}, floor_material_id{r1_id, 3};
constexpr EntityId floor_entity{r1_id, 0x100}, environment_entity{r1_id, 0x101}, sun_entity{r1_id, 0x102},
    spot_entity{r1_id, 0x103}, warm_entity{r1_id, 0x104}, cool_entity{r1_id, 0x105};

// The layout, in metres: the board at the origin (0.7 m square), the helmet to its left, and CesiumMan at
// half scale (0.75 m tall) walking a loop around the board.
constexpr math::Vec3 board_at{0, 0, 0}, helmet_at{-1.5f, 0, 0.1f};
constexpr float walker_scale = 0.5f, walk_radius = 0.8f;
constexpr float path_radius = 2.6f, path_height = 1.35f;
constexpr math::Vec3 path_target{0, 0.25f, 0};
// CesiumMan faces +Z, as the walk loop's facing does.
constexpr float walker_yaw = 0.0f;

constexpr std::array<View, 5> view_list{{
    {"overview", EntityId{r1_id, 0x200}, {1.5f, 1.15f, 2.3f}, {-0.45f, 0.2f, 0}},
    {"board", EntityId{r1_id, 0x201}, {0.55f, 0.45f, 0.62f}, {0, 0.06f, 0}},
    {"helmet", EntityId{r1_id, 0x202}, {-0.95f, 0.55f, 0.85f}, {-1.5f, 0.38f, 0.1f}},
    {"walker", EntityId{r1_id, 0x203}, {1.45f, 0.55f, -1.35f}, {0.8f, 0.33f, 0}},
    {"grazing", EntityId{r1_id, 0x204}, {-2.4f, 0.14f, 1.7f}, {0.5f, 0.05f, -0.3f}},
}};

math::Quat looking(const math::Vec3& from, const math::Vec3& to) {
    const auto f = (to - from).normalized();
    auto q = math::Quat::from_axis_angle({0, 1, 0}, std::atan2(-f.x, -f.z)) * math::Quat::from_axis_angle({1, 0, 0}, std::asin(f.y));
    q.normalize();
    return q;
}

std::string base64(const std::string& bytes) {
    static constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto out = std::string{};
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint8_t b0 = uint8_t(bytes[i]), b1 = i + 1 < bytes.size() ? uint8_t(bytes[i + 1]) : 0,
                      b2 = i + 2 < bytes.size() ? uint8_t(bytes[i + 2]) : 0;
        out += digits[b0 >> 2];
        out += digits[((b0 & 3) << 4) | (b1 >> 4)];
        out += i + 1 < bytes.size() ? digits[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        out += i + 2 < bytes.size() ? digits[b2 & 63] : '=';
    }
    return out;
}

/// r1_paths.gltf: a "Walk loop" node and a "Path camera", and one clip, "R1 loop", that turns both once
/// around the board in path_seconds: the walk loop at walk_radius, facing along it, and the camera at
/// path_radius and path_height, looking at the board. 24 keys a second, interpolated linearly.
std::string paths_gltf() {
    constexpr int keys = 241;
    auto times = std::vector<float>{}, walk_t = std::vector<float>{}, walk_r = std::vector<float>{}, cam_t = std::vector<float>{},
         cam_r = std::vector<float>{};
    for (int k = 0; k < keys; ++k) {
        const auto t = path_seconds * float(k) / float(keys - 1);
        const auto turn = 2.0f * math::PI * t / path_seconds; // counter-clockwise seen from above
        times.push_back(t);
        const auto walk = math::Vec3{walk_radius * std::cos(turn), 0, -walk_radius * std::sin(turn)};
        const auto along = math::Vec3{-std::sin(turn), 0, -std::cos(turn)}; // the loop's direction there
        const auto facing = math::Quat::from_axis_angle({0, 1, 0}, std::atan2(along.x, along.z));
        walk_t.insert(walk_t.end(), {walk.x, walk.y, walk.z});
        walk_r.insert(walk_r.end(), {facing.x, facing.y, facing.z, facing.w});
        const auto eye = math::Vec3{path_radius * std::sin(turn), path_height, path_radius * std::cos(turn)};
        const auto look = looking(eye, path_target);
        cam_t.insert(cam_t.end(), {eye.x, eye.y, eye.z});
        cam_r.insert(cam_r.end(), {look.x, look.y, look.z, look.w});
    }
    auto bytes = std::string{};
    auto views = std::vector<std::string>{};
    const auto view = [&](const std::vector<float>& values) {
        const auto offset = bytes.size();
        bytes.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float));
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(offset) + R"(,"byteLength":)" +
                        std::to_string(values.size() * sizeof(float)) + "}");
        return views.size() - 1;
    };
    const auto v_times = view(times), v_walk_t = view(walk_t), v_walk_r = view(walk_r), v_cam_t = view(cam_t), v_cam_r = view(cam_r);
    auto json = std::ostringstream{};
    json << R"({"asset":{"version":"2.0","generator":"maya_r1, recipe )" << recipe_version << R"("},)"
         << R"("buffers":[{"byteLength":)" << bytes.size() << R"(,"uri":"data:application/octet-stream;base64,)" << base64(bytes) << R"("}],)"
         << R"("bufferViews":[)";
    for (size_t i = 0; i < views.size(); ++i) json << (i ? "," : "") << views[i];
    const auto n = std::to_string(keys);
    json << R"(],"accessors":[)"
         << R"({"bufferView":)" << v_times << R"(,"componentType":5126,"count":)" << n << R"(,"type":"SCALAR","min":[0],"max":[)" << path_seconds << "]},"
         << R"({"bufferView":)" << v_walk_t << R"(,"componentType":5126,"count":)" << n << R"(,"type":"VEC3"},)"
         << R"({"bufferView":)" << v_walk_r << R"(,"componentType":5126,"count":)" << n << R"(,"type":"VEC4"},)"
         << R"({"bufferView":)" << v_cam_t << R"(,"componentType":5126,"count":)" << n << R"(,"type":"VEC3"},)"
         << R"({"bufferView":)" << v_cam_r << R"(,"componentType":5126,"count":)" << n << R"(,"type":"VEC4"}],)"
         << R"("cameras":[{"type":"perspective","perspective":{"yfov":0.8,"znear":0.05,"zfar":100}}],)"
         << R"("nodes":[{"name":"Walk loop"},{"name":")" << path_camera_name << R"(","camera":0}],)"
         << R"("animations":[{"name":"R1 loop","samplers":[{"input":0,"output":1},{"input":0,"output":2},{"input":0,"output":3},{"input":0,"output":4}],)"
         << R"("channels":[{"sampler":0,"target":{"node":0,"path":"translation"}},{"sampler":1,"target":{"node":0,"path":"rotation"}},)"
         << R"({"sampler":2,"target":{"node":1,"path":"translation"}},{"sampler":3,"target":{"node":1,"path":"rotation"}}]}],)"
         << R"("scenes":[{"nodes":[1,0]}],"scene":0})";
    return json.str();
}

/// An 8 m square floor at y = 0, facing up.
constexpr const char* floor_obj = "# R1's floor (maya_r1)\nv -4 0 -4\nv -4 0 4\nv 4 0 4\nv 4 0 -4\n"
                                  "vt 0 0\nvt 0 4\nvt 4 4\nvt 4 0\nvn 0 1 0\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n";

bool write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    auto output = std::ofstream(path, std::ios::binary);
    output << text;
    return bool(output.flush());
}

TransformComponent pose(const math::Vec3& from, const math::Vec3& to) { return {from, looking(from, to), {1, 1, 1}}; }

} // namespace

std::span<const View> views() { return view_list; }

bool samples_present(const fs::path& samples) {
    return fs::is_regular_file(samples / "Models/ABeautifulGame/glTF-Binary/ABeautifulGame.glb") &&
           fs::is_regular_file(samples / "Models/FlightHelmet/glTF/FlightHelmet.gltf") &&
           fs::is_regular_file(samples / "Models/CesiumMan/glTF-Binary/CesiumMan.glb") &&
           fs::is_regular_file(samples / "hdri/aerodynamics_workshop_2k.hdr");
}

Result assemble(const Options& options) {
    auto result = Result{};
    const auto fail = [&](std::string why) {
        result.error = std::move(why);
        return result;
    };
    if (!samples_present(options.samples))
        return fail("the R1 samples are missing from " + options.samples.string() + "; run tools/fetch_render_samples.sh");
    const auto& root = options.output;
    std::error_code error;
    fs::create_directories(root / "models", error);
    if (error) return fail("cannot write " + root.string() + ": " + error.message());

    // The project, a floor, and the workshop environment, under fixed IDs; the imports add their parts.
    auto ok = write_text(root / "project.maya", "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\nstartup \"r1.scene\"\n");
    ok = ok && write_text(root / "floor/floor.obj", floor_obj);
    auto floor = MaterialAsset{};
    floor.base_color = {0.42f, 0.4f, 0.37f, 1.0f};
    floor.roughness = 0.75f;
    ok = ok && write_text(root / "floor/floor.material", write_material_file(floor));
    ok = ok && write_text(root / "environments/workshop.environment",
                          "maya-environment 1\nsource \"workshop.hdr\"\nspecular_size 128\nsamples 256\n");
    if (!ok) return fail("cannot write the project's files in " + root.string());
    fs::copy_file(options.samples / "hdri/aerodynamics_workshop_2k.hdr", root / "environments/workshop.hdr",
                  fs::copy_options::update_existing, error);
    if (error) return fail("cannot copy the HDRI: " + error.message());
    {
        // Keep what earlier imports cataloged; set the authored entries.
        auto records = std::vector<AssetRecord>{};
        if (auto input = std::ifstream(root / "catalog.maya")) {
            auto read = read_asset_catalog(input);
            if (read)
                for (auto& record : read.records)
                    if (record.id.high != r1_id) records.push_back(std::move(record));
        }
        records.push_back({environment_id, AssetKind::environment, "environments/workshop.environment"});
        records.push_back({floor_mesh_id, AssetKind::mesh, "floor/floor.obj"});
        records.push_back({floor_material_id, AssetKind::material, "floor/floor.material"});
        auto output = std::ofstream(root / "catalog.maya");
        write_asset_catalog(output, records);
        if (!output.flush()) return fail("cannot write the catalog");
    }
    if (!write_text(root / "models/r1_paths.gltf", paths_gltf())) return fail("cannot write the path file");

    auto opened = open_project(root);
    if (!opened) return fail(opened.error);
    const auto any = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    // Imports each model with the given compression, and returns its scene.
    const auto import = [&](const fs::path& source, const std::string& name) -> std::optional<SceneDocument> {
        const auto target = root / "models" / name;
        if (source != target) {
            fs::copy_file(source, target, fs::copy_options::update_existing, error);
            if (error) return std::nullopt;
            if (source.extension() == ".gltf") {
                const auto file = GltfFile::open(source);
                if (!file) return std::nullopt;
                for (const auto& named : file.file->document().files) {
                    fs::create_directories((target.parent_path() / named).parent_path());
                    fs::copy_file(source.parent_path() / named, target.parent_path() / named, fs::copy_options::update_existing, error);
                    if (error) return std::nullopt;
                }
            }
        }
        // The import file's settings, kept with the IDs it records: only the compression is set.
        auto settings = std::string("maya-import 1\ncompression ") + texture_compression_name(options.compression) +
                        "\nmips on\nscale 1\nup y\nlights on\ncameras on\n";
        if (auto input = std::ifstream(target.string() + ".import")) {
            auto read = read_import_file(input);
            if (read) {
                read.file.settings.compression = options.compression;
                settings = write_import_file(read.file);
            }
        }
        if (!write_text(target.string() + ".import", settings)) return std::nullopt;
        const auto imported = import_gltf(opened.project, fs::path("models") / name);
        if (!imported) {
            result.error = name + ": " + gltf_problem_text(imported.errors.front());
            return std::nullopt;
        }
        auto scene = load_scene_file(root / imported.scene, any);
        if (!scene) return std::nullopt;
        return std::move(scene.document);
    };
    const auto models = options.samples / "Models";
    auto paths = import(root / "models/r1_paths.gltf", "r1_paths.gltf");
    auto board = import(models / "ABeautifulGame/glTF-Binary/ABeautifulGame.glb", "ABeautifulGame.glb");
    auto helmet = import(models / "FlightHelmet/glTF/FlightHelmet.gltf", "FlightHelmet.gltf");
    auto walker = import(models / "CesiumMan/glTF-Binary/CesiumMan.glb", "CesiumMan.glb");
    if (!paths || !board || !helmet || !walker) return fail(result.error.empty() ? "a model could not be imported" : result.error);

    // r1.scene: the path first, so its camera is the scene's first, then the models, the floor, the lights,
    // the environment, and the views.
    auto entities = std::vector<SceneEntity>{};
    auto walk_loop = std::optional<EntityId>{};
    for (auto& entity : paths->entities) {
        for (const auto& component : entity.components)
            if (const auto* name = std::get_if<NameComponent>(&component); name && name->value == "Walk loop") walk_loop = entity.id;
        entities.push_back(std::move(entity));
    }
    if (!walk_loop) return fail("the path file has no walk loop");
    const auto place = [&](SceneDocument& document, const math::Vec3& at, float scale, float yaw, std::optional<EntityId> parent) {
        for (auto& entity : document.entities) {
            if (!entity.parent) {
                entity.parent = parent;
                for (auto& component : entity.components)
                    if (auto* transform = std::get_if<TransformComponent>(&component)) {
                        transform->translation = at;
                        transform->rotation = math::Quat::from_axis_angle({0, 1, 0}, yaw);
                        transform->scale = math::Vec3{scale};
                    }
            }
            entities.push_back(std::move(entity));
        }
    };
    place(*board, board_at, 1.0f, 0.0f, std::nullopt);
    place(*helmet, helmet_at, 1.0f, 0.5f, std::nullopt);
    place(*walker, {0, 0, 0}, walker_scale, walker_yaw, walk_loop);
    entities.push_back({floor_entity, std::nullopt,
                        {NameComponent{"Floor"}, TransformComponent{{0, -0.002f, 0}, {}, {1, 1, 1}},
                         MeshRendererComponent{{floor_mesh_id}, {floor_material_id}, true}}});
    const auto light = [&](EntityId id, const char* name, LightComponent value, const math::Vec3& at, const math::Vec3& toward) {
        entities.push_back({id, std::nullopt, {NameComponent{name}, pose(at, toward), value}});
    };
    auto sun = LightComponent{LightKind::directional, {1.0f, 0.95f, 0.9f}, 3.0f};
    sun.shadow_distance = 12.0f;
    light(sun_entity, "Sun", sun, {2, 4, 3}, {0, 0, 0});
    light(spot_entity, "Spot", LightComponent{LightKind::spot, {1.0f}, 40.0f, 6.0f, 0.4f, 0.8f}, {0, 2.5f, 1}, {0, 0, 0});
    light(warm_entity, "Warm point", LightComponent{LightKind::point, {1.0f, 0.7f, 0.4f}, 8.0f, 5.0f}, {-1.2f, 1.0f, 0.6f}, {0, 0, 0});
    light(cool_entity, "Cool point", LightComponent{LightKind::point, {0.5f, 0.7f, 1.0f}, 8.0f, 5.0f}, {1.2f, 1.0f, 0.6f}, {0, 0, 0});
    entities.push_back({environment_entity, std::nullopt,
                        {NameComponent{"Workshop"}, EnvironmentComponent{AssetRef<EnvironmentAsset>{environment_id}, 1.0f, 0.0f, true}}});
    for (const auto& view : view_list) {
        auto lens = CameraComponent{};
        lens.vertical_fov = 0.8f;
        lens.near_clip = 0.05f;
        lens.far_clip = 100.0f;
        entities.push_back({view.camera, std::nullopt, {NameComponent{"View: " + std::string(view.name)}, pose(view.from, view.to), lens}});
    }
    result.scene = root / "r1.scene";
    if (const auto problems = save_scene_file(result.scene, SceneDocument{std::move(entities)}, any); !problems.empty())
        return fail("r1.scene: " + problems.front().message);
    result.project = root / "project.maya";
    return result;
}

} // namespace maya::r1
