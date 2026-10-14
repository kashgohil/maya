#include "w1.hpp"
#include "maya/assets/generator_file.hpp"
#include "maya/assets/gltf.hpp"
#include "maya/assets/import_file.hpp"
#include "maya/assets/material_file.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/registry.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/procedural/height_field.hpp"
#include "maya/procedural/scatter.hpp"
#include "maya/scene/scene_io.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace maya::w1 {
namespace fs = std::filesystem;
namespace {

// Fixed IDs for what the assembler authors ("W1" and an index), so the world and tests name them.
constexpr uint64_t w1_id = 0x57310000;
constexpr AssetId sky_id{w1_id, 1}, heights_id{w1_id, 2}, boulders_id{w1_id, 3}, rocks_id{w1_id, 4}, soak_rocks_id{w1_id, 5},
    deadwood_id{w1_id, 6}, forest_id{w1_id, 7}, rock_face_id{w1_id, 8}, watch_script_id{w1_id, 15};
// The terrain layers' maps: color, normal, and ambient occlusion-roughness-metallic, for forest then rock.
constexpr std::array<AssetId, 6> layer_maps{{{w1_id, 9}, {w1_id, 10}, {w1_id, 11}, {w1_id, 12}, {w1_id, 13}, {w1_id, 14}}};
constexpr EntityId terrain_entity{w1_id, 0x100}, sun_entity{w1_id, 0x101}, sky_entity{w1_id, 0x102}, boulders_entity{w1_id, 0x103},
    rocks_entity{w1_id, 0x104}, soak_rocks_entity{w1_id, 0x105}, deadwood_entity{w1_id, 0x106}, clearing_entity{w1_id, 0x107},
    soak_box_entity{w1_id, 0x108}, watch_entity{w1_id, 0x109};
constexpr uint64_t resting_rocks_first = 0x1000, rolling_boulders_first = 0x2000; // a body, then its mesh below it

// The layout, in metres (docs/acceptance.md#w1): the clearing on the flattest ground near the middle, with the
// board and the helmet at its west side and CesiumMan's loop east of them around a cell corner's two edges
// (cells 42_43, 43_43, and 42_44), and S1's walk east from it along z = 5,640 over the hill, then its flyover
// north-east. The sun is toward -z and +x, so the views look away from it.
constexpr double clearing_x = 5452.0, clearing_z = 5630.0; // the board; the helmet 4.5 m east and 2 m south of it
constexpr double loop_x = 5484.0, loop_z = 5612.0, loop_radius = 24.0; // 20 m from each edge, 28 m from the corner
/// Where on the loop CesiumMan starts: its point nearest the board, 13 m from it.
const double loop_start = std::atan2(-(clearing_z - loop_z), clearing_x - loop_x);
constexpr float walk_speed = 1.2f; // m/s around the loop
constexpr float landmark_scale = 3.0f; // the board and the helmet, large enough to see from the clearing
constexpr double s1_start_x = 5464.0, s1_z = 5640.0;
constexpr std::array<double, 4> s1_legs{400.0, -160.0, 240.0, -160.0}; // metres along x; each turn-back crosses a cell edge
constexpr double s1_walk_speed = 8.0, s1_flight_speed = 60.0; // m/s
constexpr float s1_flight_pitch = 0.26f; // radians below level
constexpr size_t resting_rocks = 400, rolling_boulders = 20;
// Every camera's exposure (EV100): a stop above R1's indoor 0, for a sunlit scene lit by the sun and the sky.
constexpr float camera_exposure = 1.0f;

// The scatter models (tools/fetch_world_samples.sh), and which of a set's meshes scatter uses.
constexpr std::array<std::string_view, 6> scatter_models{"boulder_01", "namaqualand_boulder_02", "rock_moss_set_01",
                                                         "rock_moss_set_02", "tree_stump_01", "dead_tree_trunk_02"};

math::Quat looking(const math::Vec3& direction) {
    const auto f = direction.normalized();
    auto q = math::Quat::from_axis_angle({0, 1, 0}, std::atan2(-f.x, -f.z)) * math::Quat::from_axis_angle({1, 0, 0}, std::asin(f.y));
    q.normalize();
    return q;
}
math::Vec3 offset(const math::DVec3& from, const math::DVec3& to) {
    return {float(to.x - from.x), float(to.y - from.y), float(to.z - from.z)};
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

bool write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    auto output = std::ofstream(path, std::ios::binary);
    output << text;
    return bool(output.flush());
}

/// SplitMix64: the same numbers on every platform, unlike the standard library's distributions.
struct Random {
    uint64_t state;
    uint64_t next() {
        auto z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform(double low, double high) { return low + (high - low) * double(next() >> 11) * 0x1.0p-53; }
};

/// W1's ground, on the triangles terrain draws, from the tiles it generates.
class Ground {
public:
    float height(double x, double z) { return tile_height_at(tile(x, z), x - corner(x), z - corner(z)); }
    float slope(double x, double z) { return tile_slope_at(tile(x, z), x - corner(x), z - corner(z)); }
    /// The highest ground under a square `half` metres either way of (x, z): what something flat rests on.
    float highest(double x, double z, double half) {
        auto top = height(x, z);
        for (const auto dx : {-half, half})
            for (const auto dz : {-half, half}) top = std::max(top, height(x + dx, z + dz));
        return top;
    }

private:
    static double corner(double v) { return std::floor(v / tile_size) * tile_size; }
    const HeightTile& tile(double x, double z) {
        const auto index = TileIndex{int32_t(std::floor(x / tile_size)), int32_t(std::floor(z / tile_size))};
        auto found = m_tiles.find(index);
        if (found == m_tiles.end()) found = m_tiles.emplace(index, *generate_height_tile(HeightfieldNoise{}, index)).first;
        return found->second;
    }
    std::map<TileIndex, HeightTile> m_tiles;
};

Ground& ground() {
    static auto shared = Ground{};
    return shared;
}

/// A glTF file of one node, optionally a camera, and one clip moving it: translations and rotations at the
/// given times, interpolated linearly.
std::string clip_gltf(std::string_view node, std::optional<std::array<float, 3>> camera, std::string_view clip, const std::vector<float>& times,
                      const std::vector<float>& translations, const std::vector<float>& rotations) {
    auto bytes = std::string{};
    auto views = std::vector<std::string>{};
    const auto view = [&](const std::vector<float>& values) {
        const auto offset = bytes.size();
        bytes.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float));
        views.push_back(R"({"buffer":0,"byteOffset":)" + std::to_string(offset) + R"(,"byteLength":)" +
                        std::to_string(values.size() * sizeof(float)) + "}");
        return views.size() - 1;
    };
    const auto v_times = view(times), v_t = view(translations), v_r = view(rotations);
    const auto n = std::to_string(times.size());
    auto json = std::ostringstream{};
    json.precision(9); // a float's every digit
    json << R"({"asset":{"version":"2.0","generator":"maya_w1, recipe )" << recipe_version << R"("},)"
         << R"("buffers":[{"byteLength":)" << bytes.size() << R"(,"uri":"data:application/octet-stream;base64,)" << base64(bytes) << R"("}],)"
         << R"("bufferViews":[)";
    for (size_t i = 0; i < views.size(); ++i) json << (i ? "," : "") << views[i];
    json << R"(],"accessors":[)"
         << R"({"bufferView":)" << v_times << R"(,"componentType":5126,"count":)" << n << R"(,"type":"SCALAR","min":[0],"max":[)" << times.back() << "]},"
         << R"({"bufferView":)" << v_t << R"(,"componentType":5126,"count":)" << n << R"(,"type":"VEC3"},)"
         << R"({"bufferView":)" << v_r << R"(,"componentType":5126,"count":)" << n << R"(,"type":"VEC4"}],)";
    if (camera)
        json << R"("cameras":[{"type":"perspective","perspective":{"yfov":)" << (*camera)[0] << R"(,"znear":)" << (*camera)[1] << R"(,"zfar":)"
             << (*camera)[2] << "}}],";
    // At rest where its clip starts, so whatever reads its pose before the first tick (a loading screen streaming
    // around the camera) sees it there.
    json << R"("nodes":[{"name":")" << node << '"' << (camera ? R"(,"camera":0)" : "") << R"(,"translation":[)" << translations[0] << ','
         << translations[1] << ',' << translations[2] << R"(],"rotation":[)" << rotations[0] << ',' << rotations[1] << ',' << rotations[2] << ','
         << rotations[3] << "]}],"
         << R"("animations":[{"name":")" << clip << R"(","samplers":[{"input":0,"output":1},{"input":0,"output":2}],)"
         << R"("channels":[{"sampler":0,"target":{"node":0,"path":"translation"}},{"sampler":1,"target":{"node":0,"path":"rotation"}}]}],)"
         << R"("scenes":[{"nodes":[0]}],"scene":0})";
    return json.str();
}

/// w1_s1.gltf: S1's camera along its path, a key every tick.
std::string s1_gltf() {
    auto times = std::vector<float>{}, translations = std::vector<float>{}, rotations = std::vector<float>{};
    for (uint32_t tick = 0; tick <= s1_ticks; ++tick) {
        const auto point = s1_point(tick);
        const auto y = point.on_foot ? ground().height(point.x, point.z) + s1_eye_height : s1_flight_height;
        const auto turn = looking(point.forward);
        times.push_back(float(tick) / 60.0f);
        translations.insert(translations.end(), {float(point.x), y, float(point.z)});
        rotations.insert(rotations.end(), {turn.x, turn.y, turn.z, turn.w});
    }
    return clip_gltf(s1_camera_name, std::array{0.9f, 0.1f, 5000.0f}, "S1", times, translations, rotations);
}

/// w1_walk.gltf: the walk loop, around its anchor at (loop_x, 0, loop_z), at the terrain's height, facing along
/// the loop; ten keys a second.
std::string walk_gltf() {
    const auto period = float(2.0 * math::PI * loop_radius) / walk_speed;
    const auto keys = int(std::ceil(period * 10.0f)) + 1;
    auto times = std::vector<float>{}, translations = std::vector<float>{}, rotations = std::vector<float>{};
    for (int k = 0; k < keys; ++k) {
        const auto t = period * float(k) / float(keys - 1);
        const auto turn = loop_start + 2.0 * math::PI * double(t / period); // counter-clockwise seen from above
        const auto dx = loop_radius * std::cos(turn), dz = -loop_radius * std::sin(turn);
        const auto along = math::Vec3{-float(std::sin(turn)), 0, -float(std::cos(turn))};
        const auto facing = math::Quat::from_axis_angle({0, 1, 0}, std::atan2(along.x, along.z));
        times.push_back(t);
        translations.insert(translations.end(), {float(dx), ground().height(loop_x + dx, loop_z + dz), float(dz)});
        rotations.insert(rotations.end(), {facing.x, facing.y, facing.z, facing.w});
    }
    return clip_gltf("Walk loop", std::nullopt, "W1 walk", times, translations, rotations);
}

constexpr auto watch_script = R"(-- W1's watch camera (#1069): turns each tick toward the entity it watches, CesiumMan's walk loop, a weak
-- reference into a cell. While that cell is not in, the reference does not resolve and the camera holds still.
local Watch = {}

Watch.properties = {
    target = { type = "entity", label = "Watched" },
    height = { type = "number", default = 1, unit = "m", label = "Aim above" },
}

function Watch:fixed_update(dt: number)
    local target = self.target
    if target == nil or not target:alive() then
        return
    end
    local f = vector.normalize((target:world_position() - self.entity:world_position()) + vector.create(0, self.height, 0))
    local yaw = maya.quaternion.from_axis_angle(vector.create(0, 1, 0), math.atan2(-f.x, -f.z))
    local pitch = maya.quaternion.from_axis_angle(vector.create(1, 0, 0), math.asin(f.y))
    self.entity:set_rotation((yaw * pitch):normalized())
end

return Watch
)";

GeneratorAsset scatter(int32_t seed, float spacing, float density, float max_slope, float min_scale, float max_scale) {
    auto generator = GeneratorAsset{};
    generator.kind = GeneratorKind::scatter;
    generator.version = scatter_version;
    generator.seed = seed;
    generator.heights = {heights_id};
    generator.spacing = spacing;
    generator.density = density;
    generator.max_slope = max_slope;
    generator.min_scale = min_scale;
    generator.max_scale = max_scale;
    generator.region_min_x = generator.region_min_z = first_cell;
    generator.region_max_x = generator.region_max_z = last_cell;
    return generator;
}

/// A mesh of an imported model and the material its scene draws it with.
struct Part {
    AssetId mesh, material;
};

/// A view: its camera `eye` metres over the ground at (x, z), looking at a point `aim` metres over the ground
/// at (tx, tz).
View ground_view(std::string_view name, uint64_t index, double x, double z, float eye, double tx, double tz, float aim) {
    return {name, EntityId{w1_id, 0x200 + index}, {x, ground().height(x, z) + eye, z}, {tx, ground().height(tx, tz) + aim, tz}};
}

std::vector<View> make_views() {
    auto list = std::vector<View>{};
    // The board, the helmet, and CesiumMan where he starts his loop.
    list.push_back(ground_view("clearing", 0, 5471.0, 5614.0, 2.2f, 5458.0, 5626.0, 0.8f));
    // The steepest ground near S1's walk, its sunlit face.
    list.push_back(ground_view("hillside", 1, 5898.0, 5320.0, 3.0f, 5898.0, 5400.0, 8.0f));
    // From the highest ground, 2 km across the world toward its middle.
    list.push_back(ground_view("vista", 2, 4876.0, 4780.0, 3.0f, 6238.0, 6245.0, 0.0f));
    // Twenty seconds into the flyover, looking ahead along it and down 35°, so the world's edge is out of sight.
    const auto fly = s1_point(s1_walk_ticks + 1200);
    const auto eye = math::DVec3{fly.x, s1_flight_height, fly.z};
    const auto ahead = 300.0 * std::sqrt(0.5);
    list.push_back({"flyover", EntityId{w1_id, 0x203}, eye, {eye.x + ahead, eye.y - 300.0 * std::tan(0.61), eye.z + ahead}});
    // Across a corner where four cells meet: x = 5,760 and z = 5,632.
    list.push_back(ground_view("cell-edge", 4, 5744.0, 5610.0, 2.5f, 5790.0, 5660.0, 0.0f));
    return list;
}

} // namespace

S1Point s1_point(uint32_t tick) {
    tick = std::min(tick, s1_ticks);
    auto point = S1Point{};
    if (tick <= s1_walk_ticks) {
        auto along = s1_walk_speed * double(tick) / 60.0;
        point.x = s1_start_x;
        point.z = s1_z;
        for (const auto leg : s1_legs) {
            const auto direction = leg < 0.0 ? -1.0 : 1.0;
            point.forward = {float(direction), 0.0f, 0.0f};
            if (along <= std::abs(leg)) {
                point.x += direction * along;
                return point;
            }
            point.x += leg;
            along -= std::abs(leg);
        }
        return point;
    }
    // The flyover: from where the walk ends, north-east, level.
    auto start_x = s1_start_x;
    for (const auto leg : s1_legs) start_x += leg;
    const auto along = s1_flight_speed * double(tick - s1_walk_ticks) / 60.0;
    const auto diagonal = std::sqrt(0.5);
    point.on_foot = false;
    point.x = start_x + along * diagonal;
    point.z = s1_z + along * diagonal;
    point.forward = math::Vec3{float(diagonal * std::cos(s1_flight_pitch)), -std::sin(s1_flight_pitch), float(diagonal * std::cos(s1_flight_pitch))};
    return point;
}

std::span<const View> views() {
    static const auto list = make_views();
    return list;
}

AssetId soak_rocks_generator() { return soak_rocks_id; }
EntityId watch_camera() { return watch_entity; }

std::string missing_samples(const fs::path& world_samples, const fs::path& render_samples) {
    auto files = std::vector<fs::path>{};
    for (const auto model : scatter_models) files.push_back(world_samples / "models" / model / (std::string(model) + "_2k.gltf"));
    for (const auto set : {"forest_ground_04", "rock_face_03"})
        for (const auto map : {"diff", "nor_gl", "arm"})
            files.push_back(world_samples / "textures" / set / (std::string(set) + "_" + map + "_2k.jpg"));
    files.push_back(world_samples / "hdri/kloofendal_48d_partly_cloudy_puresky_2k.hdr");
    files.push_back(render_samples / "Models/ABeautifulGame/glTF-Binary/ABeautifulGame.glb");
    files.push_back(render_samples / "Models/FlightHelmet/glTF/FlightHelmet.gltf");
    files.push_back(render_samples / "Models/CesiumMan/glTF-Binary/CesiumMan.glb");
    for (const auto& file : files)
        if (!fs::is_regular_file(file)) return file.string() + " is missing";
    return {};
}

Result assemble(const Options& options) {
    auto result = Result{};
    const auto fail = [&](std::string why) {
        result.error = std::move(why);
        return result;
    };
    if (auto missing = missing_samples(options.world_samples, options.render_samples); !missing.empty())
        return fail(missing + "; run tools/fetch_world_samples.sh and tools/fetch_render_samples.sh");
    const auto& root = options.output;
    std::error_code error;
    fs::create_directories(root / "models", error);
    if (error) return fail("cannot write " + root.string() + ": " + error.message());
    const auto copy = [&](const fs::path& from, const fs::path& to) {
        fs::create_directories(to.parent_path(), error);
        if (!error) fs::copy_file(from, to, fs::copy_options::update_existing, error);
        return !error;
    };

    // The project, with W1's streaming radii written out (they are the defaults).
    auto ok = write_text(root / "project.maya", "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\nstartup \"w1.world\"\n"
                                                "stream_load 640\nstream_activate 384\nstream_hysteresis 64\n");
    // The sky.
    ok = ok && copy(options.world_samples / "hdri/kloofendal_48d_partly_cloudy_puresky_2k.hdr", root / "sky/kloofendal.hdr");
    ok = ok && write_text(root / "sky/kloofendal.environment", "maya-environment 1\nsource \"kloofendal.hdr\"\nspecular_size 128\nsamples 256\n");
    // The terrain's layers: forest ground, and rock on slopes.
    auto records = std::vector<AssetRecord>{};
    const auto layer = [&](std::string_view set, size_t first_map, AssetId material_id) {
        const auto folder = fs::path("terrain") / set;
        const auto maps = std::array{std::pair{"diff", TextureRole::color}, std::pair{"nor_gl", TextureRole::normal}, std::pair{"arm", TextureRole::data}};
        for (size_t i = 0; i < maps.size(); ++i) {
            const auto name = std::string(set) + "_" + maps[i].first;
            ok = ok && copy(options.world_samples / "textures" / set / (name + "_2k.jpg"), root / folder / (name + ".jpg"));
            auto settings = TextureSettings{};
            settings.source = name + ".jpg";
            settings.role = maps[i].second;
            settings.compression = options.compression;
            auto text = std::ostringstream{};
            write_texture_settings(text, settings);
            ok = ok && write_text(root / folder / (name + ".texture"), text.str());
            records.push_back({layer_maps[first_map + i], AssetKind::texture, (folder / (name + ".texture")).generic_string()});
        }
        auto material = MaterialAsset{};
        material.roughness = 1.0f;
        material.base_color_texture = {layer_maps[first_map]};
        material.normal_texture = {layer_maps[first_map + 1]};
        material.metallic_roughness_texture = {layer_maps[first_map + 2]}; // ARM: roughness in green, metallic in blue
        material.occlusion_texture = {layer_maps[first_map + 2]}; // and occlusion in red
        ok = ok && write_text(root / folder / (std::string(set) + ".material"), write_material_file(material));
        records.push_back({material_id, AssetKind::material, (folder / (std::string(set) + ".material")).generic_string()});
    };
    layer("forest_ground_04", 0, forest_id);
    layer("rock_face_03", 3, rock_face_id);
    // The heights: W1's noise over the world.
    auto heights = GeneratorAsset{};
    heights.region_min_x = heights.region_min_z = first_cell;
    heights.region_max_x = heights.region_max_z = last_cell;
    ok = ok && write_text(root / "generators/terrain.generator", write_generator_file(heights));
    ok = ok && write_text(root / "scripts/watch.luau", watch_script);
    ok = ok && write_text(root / "models/w1_s1.gltf", s1_gltf()) && write_text(root / "models/w1_walk.gltf", walk_gltf());
    if (!ok) return fail("cannot write the project's files in " + root.string() + (error ? ": " + error.message() : std::string()));
    records.push_back({sky_id, AssetKind::environment, "sky/kloofendal.environment"});
    records.push_back({heights_id, AssetKind::generator, "generators/terrain.generator"});
    records.push_back({watch_script_id, AssetKind::script, "scripts/watch.luau"});
    for (const auto& [id, name] : {std::pair{boulders_id, "boulders"}, {rocks_id, "rocks"}, {soak_rocks_id, "soak_rocks"}, {deadwood_id, "deadwood"}})
        records.push_back({id, AssetKind::generator, "generators/" + std::string(name) + ".generator"});
    {
        // Keep what earlier imports cataloged; set the authored entries. The scatter generators are written once
        // their meshes' IDs are known, below.
        auto catalog = std::vector<AssetRecord>{};
        if (auto input = std::ifstream(root / "catalog.maya")) {
            auto read = read_asset_catalog(input);
            if (read)
                for (auto& record : read.records)
                    if (record.id.high != w1_id) catalog.push_back(std::move(record));
        }
        catalog.insert(catalog.end(), records.begin(), records.end());
        auto output = std::ofstream(root / "catalog.maya");
        write_asset_catalog(output, catalog);
        if (!output.flush()) return fail("cannot write the catalog");
    }

    auto opened = open_project(root);
    if (!opened) return fail(opened.error);
    const auto any = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    // Imports a model with the given compression into models/<target>, and returns its scene.
    const auto import = [&](const fs::path& source, const fs::path& target_name) -> std::optional<SceneDocument> {
        const auto target = root / "models" / target_name;
        if (source != target) {
            if (!copy(source, target)) return std::nullopt;
            if (source.extension() == ".gltf") {
                const auto file = GltfFile::open(source);
                if (!file) return std::nullopt;
                for (const auto& named : file.file->document().files)
                    if (!copy(source.parent_path() / named, target.parent_path() / named)) return std::nullopt;
            }
        }
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
        const auto imported = import_gltf(opened.project, fs::path("models") / target_name);
        if (!imported) {
            result.error = target_name.generic_string() + ": " + gltf_problem_text(imported.errors.front());
            return std::nullopt;
        }
        auto scene = load_scene_file(root / imported.scene, any);
        if (!scene) return std::nullopt;
        return std::move(scene.document);
    };
    // The scatter models' parts, in their scenes' order.
    auto parts = std::map<std::string, std::vector<Part>, std::less<>>{};
    for (const auto model : scatter_models) {
        const auto name = std::string(model);
        auto scene = import(options.world_samples / "models" / name / (name + "_2k.gltf"), fs::path(name) / (name + "_2k.gltf"));
        if (!scene) return fail(result.error.empty() ? name + " could not be imported" : result.error);
        for (const auto& entity : scene->entities)
            for (const auto& component : entity.components)
                if (const auto* renderer = std::get_if<MeshRendererComponent>(&component))
                    parts[name].push_back({renderer->mesh.id, renderer->material.id});
        if (parts[name].empty()) return fail(name + " has no meshes");
    }
    const auto part = [&](std::string_view model, size_t index) -> std::optional<Part> {
        const auto found = parts.find(model);
        if (found == parts.end() || index >= found->second.size()) return std::nullopt;
        return found->second[index];
    };
    const auto boulder = part("boulder_01", 0), namaqualand = part("namaqualand_boulder_02", 0), stump = part("tree_stump_01", 0),
               trunk = part("dead_tree_trunk_02", 0), small_rock = part("rock_moss_set_01", 2), medium_rock = part("rock_moss_set_01", 3),
               moss_rock = part("rock_moss_set_02", 2), low_rock = part("rock_moss_set_02", 6);
    if (!boulder || !namaqualand || !stump || !trunk || !small_rock || !medium_rock || !moss_rock || !low_rock)
        return fail("a scatter model has fewer meshes than W1 uses");
    const auto variant = [](GeneratorAsset& generator, size_t index, const Part& part, float collider) {
        const auto mesh = AssetRef<MeshAsset>{part.mesh};
        const auto material = AssetRef<MaterialAsset>{part.material};
        switch (index) {
        case 0: generator.mesh0 = mesh, generator.material0 = material, generator.collider0 = collider; break;
        case 1: generator.mesh1 = mesh, generator.material1 = material, generator.collider1 = collider; break;
        case 2: generator.mesh2 = mesh, generator.material2 = material, generator.collider2 = collider; break;
        default: generator.mesh3 = mesh, generator.material3 = material, generator.collider3 = collider; break;
        }
    };
    // The scatter sets (recipe version 2): boulders on slopes to 35°, rocks everywhere, deadwood; colliders on
    // the boulders and the deadwood, by their meshes' size.
    auto boulders = scatter(3, 8.0f, 0.44f, 35.0f, 0.8f, 1.6f);
    variant(boulders, 0, *boulder, 0.8f);
    variant(boulders, 1, *namaqualand, 0.7f);
    auto rocks = scatter(4, 4.0f, 0.4f, 90.0f, 0.25f, 0.6f);
    variant(rocks, 0, *small_rock, 0.0f);
    variant(rocks, 1, *medium_rock, 0.0f);
    variant(rocks, 2, *moss_rock, 0.0f);
    variant(rocks, 3, *low_rock, 0.0f);
    auto soak_rocks = rocks; // the same rules over the soak's 2 x 2 cells, which the main rocks leave clear
    soak_rocks.seed = 5;
    soak_rocks.region_min_x = soak_first_cell.x;
    soak_rocks.region_min_z = soak_first_cell.z;
    soak_rocks.region_max_x = soak_last_cell.x;
    soak_rocks.region_max_z = soak_last_cell.z;
    auto deadwood = scatter(6, 8.0f, 0.19f, 90.0f, 0.8f, 1.2f);
    variant(deadwood, 0, *stump, 0.5f);
    variant(deadwood, 1, *trunk, 0.5f);
    for (const auto& [generator, name] :
         {std::pair{&boulders, "boulders"}, {&rocks, "rocks"}, {&soak_rocks, "soak_rocks"}, {&deadwood, "deadwood"}})
        if (!write_text(root / "generators" / (std::string(name) + ".generator"), write_generator_file(*generator)))
            return fail("cannot write the scatter generators");

    // The landmarks (R1's models), the paths, and CesiumMan.
    const auto models = options.render_samples / "Models";
    auto s1 = import(root / "models/w1_s1.gltf", "w1_s1.gltf");
    auto walk = import(root / "models/w1_walk.gltf", "w1_walk.gltf");
    auto board = import(models / "ABeautifulGame/glTF-Binary/ABeautifulGame.glb", "ABeautifulGame.glb");
    auto helmet = import(models / "FlightHelmet/glTF/FlightHelmet.gltf", "FlightHelmet.gltf");
    auto walker = import(models / "CesiumMan/glTF-Binary/CesiumMan.glb", "CesiumMan.glb");
    if (!s1 || !walk || !board || !helmet || !walker) return fail(result.error.empty() ? "a model could not be imported" : result.error);

    // The world, whole: save_world puts each root in its part, the persistent one or its cell. S1's camera comes
    // first, so it is the persistent part's first camera, which the player shows.
    auto entities = std::vector<SceneEntity>{};
    for (auto& entity : s1->entities) {
        for (auto& component : entity.components)
            if (auto* lens = std::get_if<CameraComponent>(&component)) lens->exposure = camera_exposure;
        entities.push_back(std::move(entity));
    }
    const auto find_named = [](const SceneDocument& document, std::string_view name) -> std::optional<EntityId> {
        for (const auto& entity : document.entities)
            for (const auto& component : entity.components)
                if (const auto* named = std::get_if<NameComponent>(&component); named && named->value == name) return entity.id;
        return std::nullopt;
    };
    const auto walk_loop = find_named(*walk, "Walk loop");
    if (!walk_loop) return fail("the walk file has no walk loop");
    const auto place = [&](SceneDocument& document, const math::DVec3& at, float scale, float yaw, std::optional<EntityId> parent) {
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
    auto& g = ground();
    place(*walk, {loop_x, 0.0, loop_z}, 1.0f, 0.0f, std::nullopt); // the loop's keys carry the terrain's height
    place(*walker, {}, 1.0f, 0.0f, walk_loop);
    const auto board_half = 0.35 * landmark_scale;
    place(*board, {clearing_x, g.highest(clearing_x, clearing_z, board_half), clearing_z}, landmark_scale, 0.3f, std::nullopt);
    const auto helmet_x = clearing_x + 4.5, helmet_z = clearing_z - 2.0;
    place(*helmet, {helmet_x, g.highest(helmet_x, helmet_z, 0.3 * landmark_scale), helmet_z}, landmark_scale, -0.6f, std::nullopt);

    // The terrain, the scatter generators and their exclusions, the sun, and the sky: the persistent part.
    auto terrain = TerrainComponent{};
    terrain.heights = {heights_id};
    terrain.layer0 = {forest_id};
    terrain.layer1 = {rock_face_id};
    terrain.layer1_slope = {28.0f, 38.0f};
    terrain.texture_size = 8.0f;
    entities.push_back({terrain_entity, std::nullopt, {NameComponent{"Terrain"}, TransformComponent{}, terrain}});
    for (const auto& [id, name, generator] : {std::tuple{boulders_entity, "Boulders", boulders_id}, {rocks_entity, "Rocks", rocks_id},
                                              {soak_rocks_entity, "Soak rocks", soak_rocks_id}, {deadwood_entity, "Deadwood", deadwood_id}})
        entities.push_back({id, std::nullopt, {NameComponent{name}, GeneratorComponent{{generator}}}});
    auto clearing = ScatterExclusionComponent{};
    clearing.shape = ExclusionShape::sphere;
    clearing.radius = 45.0f; // the landmarks and CesiumMan's loop
    entities.push_back({clearing_entity, std::nullopt,
                        {NameComponent{"Clearing"},
                         TransformComponent{{(clearing_x + loop_x) / 2, g.height((clearing_x + loop_x) / 2, (clearing_z + loop_z) / 2),
                                             (clearing_z + loop_z) / 2},
                                            {},
                                            {1, 1, 1}},
                         clearing}});
    auto soak_box = ScatterExclusionComponent{};
    soak_box.shape = ExclusionShape::box;
    soak_box.half_extents = {64.0f * float(soak_last_cell.x - soak_first_cell.x + 1), 1000.0f, 64.0f * float(soak_last_cell.z - soak_first_cell.z + 1)};
    soak_box.generator = {rocks_id}; // only the main rocks: the soak's own cover these cells
    const auto soak_center = math::DVec3{double(soak_first_cell.x + soak_last_cell.x + 1) * 64.0, 0.0, double(soak_first_cell.z + soak_last_cell.z + 1) * 64.0};
    entities.push_back({soak_box_entity, std::nullopt, {NameComponent{"Soak cells"}, TransformComponent{soak_center, {}, {1, 1, 1}}, soak_box}});
    // The sun where the sky's is (48° up, toward -z and +x), as bright as the sky's (4.3 lux), and the sky at half
    // its strength: its sun lights everything again, unshadowed.
    auto sun = LightComponent{LightKind::directional, {1.0f, 0.96f, 0.9f}, 4.3f};
    sun.shadow_distance = 150.0f;
    entities.push_back({sun_entity, std::nullopt,
                        {NameComponent{"Sun"}, TransformComponent{{6144.0, 300.0, 6144.0}, looking({-0.377f, -0.742f, 0.554f}), {1, 1, 1}}, sun}});
    entities.push_back({sky_entity, std::nullopt,
                        {NameComponent{"Kloofendal sky"}, EnvironmentComponent{AssetRef<EnvironmentAsset>{sky_id}, 0.5f, 0.0f, true}}});

    // The watch camera, persistent as every camera is, watching the walk loop in its cell.
    {
        const auto at = math::DVec3{5452.0, g.height(5452.0, 5596.0) + 7.0, 5596.0};
        auto lens = CameraComponent{};
        lens.vertical_fov = 0.7f;
        lens.far_clip = 5000.0f;
        lens.exposure = camera_exposure;
        auto script = ScriptComponent{{watch_script_id}, {{"target", ScriptValueType::entity, *walk_loop}}};
        entities.push_back({watch_entity, std::nullopt,
                            {NameComponent{"Watch camera"},
                             TransformComponent{at, looking(offset(at, {loop_x, g.height(loop_x, loop_z) + 1.0, loop_z})), {1, 1, 1}}, lens,
                             std::move(script)}});
    }
    // The named views.
    for (const auto& view : views()) {
        auto lens = CameraComponent{};
        lens.vertical_fov = 0.9f;
        lens.far_clip = 5000.0f;
        lens.exposure = camera_exposure;
        entities.push_back({view.camera, std::nullopt,
                            {NameComponent{"View: " + std::string(view.name)}, TransformComponent{view.from, looking(offset(view.from, view.to)), {1, 1, 1}},
                             lens}});
    }

    // Physics near S1's walk: 400 rocks resting on gentle ground, and 20 boulders on slopes of 25° to 35° that
    // roll down when their cells activate. Each is a unit-scale body with its collider, and its mesh, scaled, on
    // an entity below it.
    auto random = Random{0x57310003ull}; // fixed since version 3, so a later version does not move them
    auto placed = std::vector<std::pair<double, double>>{};
    const auto clear_of = [&](double x, double z, double spacing) {
        return std::ranges::none_of(placed, [&](const auto& p) { return std::hypot(p.first - x, p.second - z) < spacing; });
    };
    const auto body = [&](uint64_t index, const char* name, const math::DVec3& at, float yaw, ColliderComponent collider, const Part& mesh,
                          float scale) {
        auto rigid = RigidBodyComponent{};
        rigid.density = 2600.0f; // rock
        entities.push_back({EntityId{w1_id, index}, std::nullopt,
                            {NameComponent{name + std::to_string(index & 0xfff)},
                             TransformComponent{at, math::Quat::from_axis_angle({0, 1, 0}, yaw), {1, 1, 1}}, rigid, collider}});
        entities.push_back({EntityId{w1_id, index | 0x8000}, EntityId{w1_id, index},
                            {NameComponent{"Mesh"}, TransformComponent{{}, {}, math::Vec3{scale}},
                             MeshRendererComponent{{mesh.mesh}, {mesh.material}, true}}});
    };
    for (size_t attempts = 0, count = 0; count < resting_rocks && attempts < 200000; ++attempts) {
        const auto x = random.uniform(s1_start_x + 8.0, 5944.0), z = random.uniform(s1_z - 40.0, s1_z + 40.0);
        if (std::hypot(x - clearing_x, z - clearing_z) < 10.0 || std::abs(std::hypot(x - loop_x, z - loop_z) - loop_radius) < 3.0) continue;
        if (std::abs(z - s1_z) < 2.0 || g.slope(x, z) > 15.0f || !clear_of(x, z, 2.0)) continue;
        // rock_moss_set_01's small rock at 0.3: about 0.63 x 0.33 x 0.62 m, its middle 4 cm above its origin.
        auto collider = ColliderComponent{};
        collider.shape = ColliderShape::box;
        collider.half_extents = {0.317f, 0.165f, 0.312f};
        collider.offset = {0.011f, 0.042f, 0.006f};
        collider.friction = 0.8f;
        body(resting_rocks_first + count, "Resting rock ", {x, g.highest(x, z, 0.32) + 0.13, z}, float(random.uniform(0.0, 2.0 * math::PI)),
             collider, *small_rock, 0.3f);
        placed.push_back({x, z});
        ++count;
    }
    for (size_t attempts = 0, count = 0; count < rolling_boulders && attempts < 200000; ++attempts) {
        const auto x = random.uniform(s1_start_x + 16.0, 5944.0), z = random.uniform(s1_z - 80.0, s1_z + 80.0);
        const auto slope = g.slope(x, z);
        if (slope < 25.0f || slope > 35.0f || !clear_of(x, z, 15.0)) continue;
        // boulder_01 at full size: a sphere of 0.85 m about its middle, 0.43 m above its origin.
        auto collider = ColliderComponent{};
        collider.shape = ColliderShape::sphere;
        collider.radius = 0.85f;
        collider.offset = {-0.115f, 0.43f, -0.035f};
        collider.friction = 0.7f;
        body(rolling_boulders_first + count, "Rolling boulder ", {x, g.highest(x, z, 0.9) + 0.5, z}, float(random.uniform(0.0, 2.0 * math::PI)),
             collider, *boulder, 1.0f);
        placed.push_back({x, z});
        ++count;
    }

    // A cairn in every cell (version 3, decided 14 October 2026): rocks piled about a point the cell's seed chooses,
    // clear of the clearing, S1's walk, and steep ground, each a static body (its box collider, its scale baked in).
    struct Rock {
        const Part* part;
        math::Vec3 low, high; // the mesh's bounds
    };
    const auto rocks_for_cairns = std::array<Rock, 4>{{{&*small_rock, {-1.02f, -0.41f, -1.02f}, {1.09f, 0.69f, 1.06f}},
                                                       {&*medium_rock, {-0.95f, -0.66f, -0.89f}, {1.18f, 1.11f, 1.08f}},
                                                       {&*moss_rock, {-0.76f, -0.03f, -0.73f}, {0.82f, 0.7f, 0.72f}},
                                                       {&*low_rock, {-1.31f, -0.47f, -0.9f}, {1.18f, 0.63f, 0.89f}}}};
    for (auto cz = first_cell; cz <= last_cell; ++cz)
        for (auto cx = first_cell; cx <= last_cell; ++cx) {
            const auto cell = uint64_t(cz - first_cell) * uint64_t(last_cell - first_cell + 1) + uint64_t(cx - first_cell);
            auto seeded = Random{0x43414952ull * 1000003ull + cell}; // "CAIR"
            auto at = std::optional<std::pair<double, double>>{};
            for (int attempt = 0; attempt < 64 && !at; ++attempt) {
                const auto x = double(cx) * tile_size + seeded.uniform(16.0, tile_size - 16.0);
                const auto z = double(cz) * tile_size + seeded.uniform(16.0, tile_size - 16.0);
                const auto walk = x > s1_start_x - 16.0 && x < 5960.0 && std::abs(z - s1_z) < 12.0;
                if (std::hypot(x - (clearing_x + loop_x) / 2, z - (clearing_z + loop_z) / 2) < 55.0 || walk || g.slope(x, z) > 25.0f ||
                    !clear_of(x, z, 8.0)) // clear of the resting rocks and rolling boulders
                    continue;
                at = std::pair{x, z};
            }
            if (!at) at = std::pair{double(cx) * tile_size + 64.0, double(cz) * tile_size + 64.0};
            for (size_t i = 0; i < cairn_rocks; ++i) {
                const auto& rock = rocks_for_cairns[seeded.next() % rocks_for_cairns.size()];
                const auto scale = float(seeded.uniform(0.4, 0.9));
                const auto angle = seeded.uniform(0.0, 2.0 * math::PI), reach = seeded.uniform(0.0, 4.0);
                const auto x = at->first + reach * std::cos(angle), z = at->second + reach * std::sin(angle);
                // Resting on the ground under it, a tenth of its height sunk in.
                const auto y = double(g.highest(x, z, 0.5 * double(scale))) - double(rock.low.y * scale) - 0.1 * double((rock.high.y - rock.low.y) * scale);
                auto collider = ColliderComponent{};
                collider.shape = ColliderShape::box;
                collider.half_extents = (rock.high - rock.low) * 0.5f;
                collider.offset = (rock.high + rock.low) * 0.5f;
                entities.push_back({EntityId{w1_id, 0x10000 + cell * cairn_rocks + i}, std::nullopt,
                                    {NameComponent{"Cairn " + std::to_string(cx) + "_" + std::to_string(cz) + " rock " + std::to_string(i)},
                                     TransformComponent{{x, y, z}, math::Quat::from_axis_angle({0, 1, 0}, float(seeded.uniform(0.0, 2.0 * math::PI))),
                                                        math::Vec3{scale}},
                                     MeshRendererComponent{{rock.part->mesh}, {rock.part->material}, true}, collider}});
            }
        }

    // Saved as a world: the old parts go first, so a cell no longer used does not linger.
    auto document = SceneDocument{std::move(entities)};
    const auto partitioned = partition_world(document);
    result.cells = partitioned.cells.size();
    result.entities = document.entities.size();
    fs::remove_all(root / "w1", error);
    result.world = root / "w1.world";
    if (const auto problems = save_world(result.world, std::move(document), any); !problems.empty())
        return fail("w1.world: " + problems.front().message);
    result.project = root / "project.maya";
    return result;
}

} // namespace maya::w1
