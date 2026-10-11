#include "maya/assets/generator_file.hpp"
#include "maya/procedural/height_field.hpp"
#include "maya/procedural/scatter.hpp"
#include "maya/properties/schema.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <functional>
#include <set>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>

using namespace maya;

namespace {
const auto w1 = HeightfieldNoise{990, 6, 900.0f, 140.0f, 120.0f, 0.0f}; // W1's terrain (#1060's record)
bool same_bits(std::span<const float> a, std::span<const float> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}
} // namespace

TEST_CASE("Noise heights depend only on the seed, the parameters, and the position", "[procedural][noise]") {
    for (const auto& [x, z] : {std::pair<int64_t, int64_t>{0, 0}, {5000, -7000}, {8192, 4096}, {-123456, 654321}}) {
        const auto h = noise_height(w1, x, z);
        CHECK(std::isfinite(h));
        CHECK(std::abs(h) < w1.amplitude * 2.0f); // fBm of six octaves stays within twice the amplitude
        CHECK(std::bit_cast<uint32_t>(noise_height(w1, x, z)) == std::bit_cast<uint32_t>(h));
    }
    auto other = w1;
    other.seed = 991;
    auto differing = 0;
    for (int64_t x = 0; x < 2000; x += 97) differing += noise_height(w1, x, 333) != noise_height(other, x, 333);
    CHECK(differing > 15);
    // Continuous: a metre apart, heights differ by a slope, not a jump.
    for (int64_t x = 4096; x < 4196; ++x) CHECK(std::abs(noise_height(w1, x + 1, 5000) - noise_height(w1, x, 5000)) < 3.0f);
    auto raised = w1;
    raised.base = 25.0f;
    CHECK(noise_height(raised, 300, 400) == noise_height(w1, 300, 400) + 25.0f);
}

TEST_CASE("Height tiles are the same however they are generated, and neighbours share their edges", "[procedural][tiles]") {
    const auto index = TileIndex{40, 33}; // inside W1, 5.1 km and 4.2 km from the origin
    const auto serial = generate_height_tile(w1, index);
    REQUIRE(serial);
    CHECK(serial->index == index);
    REQUIRE(serial->heights.size() == size_t(tile_samples) * tile_samples);
    REQUIRE(serial->normals.size() == serial->heights.size());
    CHECK(serial->height(0, 0) == noise_height(w1, 40 * 128, 33 * 128));
    CHECK(serial->height(128, 7) == noise_height(w1, 40 * 128 + 128, 33 * 128 + 7));
    CHECK(serial->min == *std::ranges::min_element(serial->heights));
    CHECK(serial->max == *std::ranges::max_element(serial->heights));
    // Rows split across the job system's workers, again and again: the same bytes.
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto parallel = generate_height_tile(w1, index, nullptr, true);
        REQUIRE(parallel);
        CHECK(same_bits(parallel->heights, serial->heights));
        CHECK(parallel->normals == serial->normals);
        CHECK(encode_height_tile(*parallel) == encode_height_tile(*serial));
    }
    // The next tile east and the next north repeat its last column and row, heights and normals.
    const auto east = generate_height_tile(w1, {41, 33}), north = generate_height_tile(w1, {40, 34});
    REQUIRE(east);
    REQUIRE(north);
    for (uint32_t i = 0; i < tile_samples; ++i) {
        CHECK(east->height(0, i) == serial->height(tile_samples - 1, i));
        CHECK(east->normals[size_t(i) * tile_samples] == serial->normals[size_t(i) * tile_samples + tile_samples - 1]);
        CHECK(north->height(i, 0) == serial->height(i, tile_samples - 1));
        CHECK(north->normals[i] == serial->normals[size_t(tile_samples - 1) * tile_samples + i]);
    }
    // Normals point up on this rolling ground, and tilt against the slope.
    const auto decode = [](uint16_t packed) {
        const auto u = float(packed & 0xff) / 255.0f * 2.0f - 1.0f, v = float(packed >> 8) / 255.0f * 2.0f - 1.0f;
        return std::pair{u, v};
    };
    for (uint32_t i = 1; i + 1 < tile_samples; i += 16) {
        const auto [u, v] = decode(serial->normals[size_t(i) * tile_samples + i]);
        CHECK(std::abs(u) + std::abs(v) < 1.0f); // the upper hemisphere
        const auto slope_x = serial->height(i + 1, i) - serial->height(i - 1, i);
        if (std::abs(slope_x) > 0.2f) CHECK((u < 0.0f) == (slope_x > 0.0f));
    }
}

TEST_CASE("A cancelled tile is not generated", "[procedural][tiles]") {
    std::atomic<bool> go{false};
    auto result = std::optional<HeightTile>{};
    auto ran = std::atomic<bool>{false};
    const auto job = job_system().submit(JobTier::background, [&](JobContext& context) {
        ran = true;
        while (!go) std::this_thread::yield();
        result = generate_height_tile(w1, {0, 0}, &context);
    });
    while (!ran) std::this_thread::yield();
    job.cancel();
    go = true;
    job.wait();
    CHECK_FALSE(result);
}

TEST_CASE("The overview holds the tiles' heights at every 16th metre across its region", "[procedural][tiles]") {
    const auto low = TileIndex{40, 33}, high = TileIndex{42, 34};
    const auto overview = generate_height_overview(w1, low, high);
    REQUIRE(overview);
    CHECK(overview->width == 3 * 8 + 1);
    CHECK(overview->depth == 2 * 8 + 1);
    REQUIRE(overview->heights.size() == size_t(overview->width) * overview->depth);
    for (const auto index : {TileIndex{40, 33}, TileIndex{42, 34}, TileIndex{41, 33}}) {
        const auto tile = generate_height_tile(w1, index);
        REQUIRE(tile);
        for (uint32_t z = 0; z < tile_samples; z += 16)
            for (uint32_t x = 0; x < tile_samples; x += 16) {
                const auto ox = uint32_t(index.x - low.x) * 8 + x / 16, oz = uint32_t(index.z - low.z) * 8 + z / 16;
                CHECK(overview->heights[size_t(oz) * overview->width + ox] == tile->height(x, z));
            }
    }
    const auto parallel = generate_height_overview(w1, low, high, nullptr, true);
    REQUIRE(parallel);
    CHECK(encode_height_overview(*parallel) == encode_height_overview(*overview));
}

TEST_CASE("Cooked tiles and overviews read back exactly, and damaged ones are refused", "[procedural][tiles]") {
    const auto tile = generate_height_tile(w1, {-3, 7});
    REQUIRE(tile);
    const auto bytes = encode_height_tile(*tile);
    const auto back = decode_height_tile(bytes);
    REQUIRE(back);
    CHECK(back->index == tile->index);
    CHECK(same_bits(back->heights, tile->heights));
    CHECK(back->normals == tile->normals);
    CHECK(back->min == tile->min);
    CHECK(back->max == tile->max);
    CHECK_FALSE(decode_height_tile(std::span(bytes).first(bytes.size() - 1)));
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(decode_height_tile(longer));
    auto wrong = bytes;
    wrong[0] = std::byte{0};
    CHECK_FALSE(decode_height_tile(wrong));
    auto nan = bytes;
    const auto quiet = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(nan.data() + 20, &quiet, sizeof(float));
    CHECK_FALSE(decode_height_tile(nan));
    auto outside = bytes; // a height above the recorded maximum
    const auto high = tile->max + 1.0f;
    std::memcpy(outside.data() + 20 + 4 * 100, &high, sizeof(float));
    CHECK_FALSE(decode_height_tile(outside));

    const auto overview = generate_height_overview(w1, {0, 0}, {1, 2});
    REQUIRE(overview);
    const auto cooked = encode_height_overview(*overview);
    const auto again = decode_height_overview(cooked);
    REQUIRE(again);
    CHECK(again->width == overview->width);
    CHECK(same_bits(again->heights, overview->heights));
    CHECK_FALSE(decode_height_overview(std::span(cooked).first(cooked.size() - 2)));
    auto inverted = cooked;
    const auto low_x = int32_t{5};
    std::memcpy(inverted.data() + 4, &low_x, sizeof(low_x));
    CHECK_FALSE(decode_height_overview(inverted));
}

TEST_CASE("Packed heights keep every height's bits and its normal beside it", "[procedural][tiles]") {
    const auto tile = generate_height_tile(w1, {40, 33});
    REQUIRE(tile);
    const auto packed = pack_heights(tile->heights, tile->normals);
    REQUIRE(packed.size() == tile->heights.size() * 2);
    for (size_t i = 0; i < tile->heights.size(); i += 37) {
        CHECK(std::bit_cast<float>(packed[2 * i]) == tile->heights[i]);
        CHECK(packed[2 * i + 1] == tile->normals[i]);
    }
    // Neighbours' shared edges pack to the same words, so they meet exactly on the GPU.
    const auto east = generate_height_tile(w1, {41, 33});
    REQUIRE(east);
    const auto next = pack_heights(east->heights, east->normals);
    for (uint32_t z = 0; z < tile_samples; ++z)
        CHECK(next[2 * (size_t(z) * tile_samples)] == packed[2 * (size_t(z) * tile_samples + tile_samples - 1)]);
    CHECK(oct_encode(0.0f, 1.0f, 0.0f) == uint16_t(128 | (128 << 8)));
}

TEST_CASE("Generator files round-trip every property and refuse what this build cannot generate", "[procedural][generators]") {
    auto generator = GeneratorAsset{};
    generator.seed = 1234;
    generator.octaves = 4;
    generator.wavelength = 512.5f;
    generator.amplitude = 80.0f;
    generator.warp = 0.0f;
    generator.base = -12.25f;
    generator.region_min_x = 32;
    generator.region_min_z = 32;
    generator.region_max_x = 63;
    generator.region_max_z = 63;
    const auto text = write_generator_file(generator);
    CHECK(text.starts_with("maya-generator 1\nalgorithm 1\nkind heightfield.noise\nseed 1234\n"));
    auto stream = std::istringstream(text);
    const auto read = read_generator_file(stream);
    INFO(read.error);
    REQUIRE(read);
    CHECK(read.generator == generator);
    CHECK(heightfield_noise(read.generator) == HeightfieldNoise{1234, 4, 512.5f, 80.0f, 0.0f, -12.25f});
    // Omitted properties take their defaults.
    auto minimal = std::istringstream("maya-generator 1\nalgorithm 1\n# W1\nseed 990\n");
    const auto defaults = read_generator_file(minimal);
    REQUIRE(defaults);
    CHECK(defaults.generator == GeneratorAsset{});

    const auto refused = [](const std::string& body, const std::string& why) {
        auto input = std::istringstream(body);
        const auto result = read_generator_file(input);
        INFO(body);
        CHECK_FALSE(result);
        CHECK(result.error.find(why) != std::string::npos);
    };
    const auto header = std::string("maya-generator 1\nalgorithm 1\n");
    refused("maya-generator 2\n", "version 2 is not supported");
    refused("maya-material 2\n", "Expected 'maya-generator 1'");
    refused("maya-generator 1\nseed 3\n", "names no 'algorithm'");
    refused("maya-generator 1\nalgorithm 2\n", "Algorithm version 2 of heightfield.noise is not one this build generates");
    refused(header + "algorithm 1\n", "'algorithm' appears more than once");
    refused(header + "seeds 3\n", "Unknown generator property 'seeds'");
    refused(header + "seed 3\nseed 4\n", "line 4: 'seed' appears more than once");
    refused(header + "octaves 2.5\n", "'octaves' needs one whole number");
    refused(header + "octaves 13\n", "line 3: 'octaves'");
    refused(header + "seed -1\n", "'seed'");
    refused(header + "wavelength nan\n", "'wavelength': Value is nonfinite");
    refused(header + "kind rocks\n", "'kind' needs one of its choices");
    refused(header + "kind scatter\noctaves 4\n", "line 4: 'octaves' is not a property of scatter generators");
    refused(header + "spacing 2\n", "line 3: 'spacing' is not a property of heightfield.noise generators");
    refused(header + "kind scatter\nspacing 3\n", "line 4: 'spacing': The spacing must be a power of two");
    refused(header + "kind scatter\nmesh0 6d617961\n", "'mesh0' needs 'none' or two hexadecimal asset ID words");
    refused(header + "kind scatter\nmin_scale 2\nmax_scale 1\n", "'max_scale': The largest scale is smaller than the smallest");
    refused(header + "region_max_x 4\nregion_min_x 5\n", "line 3: 'region_max_x': The region's last cell along x comes before its first");
    refused(header + "region_min_z 2000\n", "'region_min_z'");
}

TEST_CASE("Generator properties are described for the Inspector and edited atomically", "[procedural][generators]") {
    REQUIRE(generator_properties().size() == 36);
    CHECK(generator_properties(GeneratorKind::heightfield_noise).size() == 11);
    CHECK(generator_properties(GeneratorKind::scatter).size() == 32);
    for (const auto& property : generator_properties()) {
        INFO(property.name);
        CHECK(generator_property(property.name) == &property);
        CHECK(generator_property(property.id) == &property);
        CHECK_FALSE(property.label.empty());
        CHECK_FALSE(property.description.empty());
    }
    auto generator = GeneratorAsset{};
    const auto edits = std::array{PropertyEdit{generator_property("amplitude")->id, 30.0f},
                                  PropertyEdit{generator_property("octaves")->id, int32_t{0}}};
    const auto failed = edit_properties(generator, edits);
    CHECK_FALSE(failed);
    CHECK(failed.property == generator_property("octaves")->id);
    CHECK(generator.amplitude == 140.0f); // nothing changed
    const auto good = std::array{PropertyEdit{generator_property("amplitude")->id, 30.0f}};
    REQUIRE(edit_properties(generator, good));
    CHECK(generator.amplitude == 30.0f);
}

// Scatter (#1093, docs/procedural.md#scatter).
namespace {
constexpr std::array<uint64_t, 2> scatter_generator{0x5ca7, 0x7e2};
ScatterRules w1_rules() { // #1060's record: 4 m spacing, density 0.6, slopes up to 30 degrees
    auto rules = ScatterRules{};
    rules.seed = 1093;
    rules.spacing = 4.0;
    rules.density = 0.6f;
    rules.max_slope = 30.0f;
    rules.weights = {3.0f, 1.0f, 0.0f, 1.0f};
    return rules;
}
std::set<EntityId> ids_of(const std::vector<ScatterItem>& items) {
    auto ids = std::set<EntityId>{};
    for (const auto& item : items) ids.insert(item.id);
    return ids;
}
} // namespace

TEST_CASE("Scatter is the same however it runs, and keeps its spacing and its rules", "[procedural][scatter]") {
    const auto tile = generate_height_tile(HeightfieldNoise{}, {40, 33});
    REQUIRE(tile);
    const auto rules = w1_rules();
    auto clearing = ScatterExclusion{};
    clearing.shape = ScatterExclusion::Shape::sphere;
    clearing.pose.translation = {40 * 128.0 + 64.0, 0.0, 33 * 128.0 + 64.0};
    clearing.radius = 20.0f;
    const auto exclusions = std::vector{clearing};
    const auto items = scatter_cell(scatter_generator, rules, {40, 33}, &*tile, exclusions);
    REQUIRE(items);
    INFO(items->size() << " items");
    CHECK(items->size() > 300);
    CHECK(items->size() < 1024 * 6 / 10);
    // On jobs, in parallel, it is the same, byte for byte.
    auto results = std::vector<std::vector<std::byte>>(8);
    job_system().parallel_for(JobTier::frame, 8, 1, [&](uint32_t begin, uint32_t end) {
        for (auto i = begin; i < end; ++i) results[i] = encode_scatter_items(*scatter_cell(scatter_generator, rules, {40, 33}, &*tile, exclusions));
    });
    for (const auto& bytes : results) CHECK(bytes == encode_scatter_items(*items));
    // Every rule holds: in the cell, on the ground, apart, within the slope, outside the clearing, never variant 2.
    auto closest = 1e9;
    for (size_t i = 0; i < items->size(); ++i) {
        const auto& item = (*items)[i];
        const auto local_x = item.position.x - 40 * 128.0, local_z = item.position.z - 33 * 128.0;
        CHECK(local_x >= 0.0);
        CHECK(local_x < 128.0);
        CHECK(item.position.y == double(tile_height_at(*tile, local_x, local_z)));
        CHECK(tile_slope_at(*tile, local_x, local_z) <= 30.0f);
        CHECK_FALSE(clearing.contains(item.position));
        CHECK(item.variant != 2);
        CHECK(item.scale >= 0.8f);
        CHECK(item.scale <= 1.2f);
        for (size_t j = i + 1; j < items->size(); ++j)
            closest = std::min(closest, std::hypot(item.position.x - (*items)[j].position.x, item.position.z - (*items)[j].position.z));
    }
    CHECK(closest >= rules.spacing * 0.5);
    // A box clears too, in its own frame: rotated, it clears a rotated square.
    auto box = ScatterExclusion{};
    box.pose.translation = {40 * 128.0 + 30.0, 0.0, 33 * 128.0 + 30.0};
    box.pose.linear = math::Quat::from_axis_angle({0, 1, 0}, 0.7f).to_mat4();
    box.half_extents = {10.0f, 500.0f, 3.0f};
    const auto boxed = scatter_cell(scatter_generator, rules, {40, 33}, &*tile, std::vector{box});
    REQUIRE(boxed);
    for (const auto& item : *boxed) CHECK_FALSE(box.contains(item.position));
    CHECK(box.contains(box.pose.point({9.0f, 0.0f, 2.5f})));
    CHECK_FALSE(box.contains(box.pose.point({9.0f, 0.0f, 3.5f})));
    // Without a height tile, a flat ground at the base height.
    auto flat = rules;
    flat.base_height = 7.5f;
    const auto ground = scatter_cell(scatter_generator, flat, {0, 0}, nullptr, {});
    REQUIRE(ground);
    CHECK_FALSE(ground->empty());
    for (const auto& item : *ground) CHECK(item.position.y == 7.5);
}

TEST_CASE("Rules only remove scatter's items: the survivors keep their IDs and places, and IDs are never shared", "[procedural][scatter]") {
    const auto tile = generate_height_tile(HeightfieldNoise{}, {40, 33});
    REQUIRE(tile);
    const auto rules = w1_rules();
    const auto before = *scatter_cell(scatter_generator, rules, {40, 33}, &*tile, {});
    auto clearing = ScatterExclusion{};
    clearing.shape = ScatterExclusion::Shape::sphere;
    clearing.pose.translation = {40 * 128.0 + 64.0, 0.0, 33 * 128.0 + 64.0};
    clearing.radius = 40.0f;
    for (const auto& [what, change] : std::vector<std::pair<std::string, std::function<void(ScatterRules&)>>>{
             {"slope 30 to 20", [](ScatterRules& r) { r.max_slope = 20.0f; }},
             {"density 0.6 to 0.3", [](ScatterRules& r) { r.density = 0.3f; }},
             {"heights above 0", [](ScatterRules& r) { r.min_height = 0.0f; }}}) {
        INFO(what);
        auto changed = rules;
        change(changed);
        const auto after = *scatter_cell(scatter_generator, changed, {40, 33}, &*tile, {});
        CHECK(after.size() < before.size());
        for (const auto& item : after) CHECK(std::ranges::find(before, item) != before.end()); // same ID, place, yaw, scale, mesh
    }
    const auto cleared = *scatter_cell(scatter_generator, rules, {40, 33}, &*tile, std::vector{clearing});
    for (const auto& item : cleared) CHECK(std::ranges::find(before, item) != before.end());
    // A new seed moves the candidates but not their IDs: every ID is still one of the cell's squares'.
    auto reseeded = rules;
    reseeded.seed = 7;
    reseeded.density = 1.0f;
    reseeded.max_slope = 90.0f;
    auto all = rules;
    all.density = 1.0f;
    all.max_slope = 90.0f;
    CHECK(ids_of(*scatter_cell(scatter_generator, reseeded, {40, 33}, &*tile, {})) == ids_of(*scatter_cell(scatter_generator, all, {40, 33}, &*tile, {})));
    // Neighbouring cells, and another generator in the same cell, share no ID.
    const auto right = generate_height_tile(HeightfieldNoise{}, {41, 33});
    const auto next = ids_of(*scatter_cell(scatter_generator, rules, {41, 33}, &*right, {}));
    const auto other = ids_of(*scatter_cell({0x5ca7, 0x7e3}, rules, {40, 33}, &*tile, {}));
    for (const auto& item : before) {
        CHECK_FALSE(next.contains(item.id));
        CHECK_FALSE(other.contains(item.id));
    }
    // Cooked items read back exactly; damaged bytes are refused.
    const auto bytes = encode_scatter_items(before);
    REQUIRE(decode_scatter_items(bytes));
    CHECK(*decode_scatter_items(bytes) == before);
    CHECK_FALSE(decode_scatter_items(std::span(bytes).first(bytes.size() - 1)));
    auto damaged = bytes;
    damaged[0] = std::byte{0};
    CHECK_FALSE(decode_scatter_items(damaged));
    auto meshless = rules;
    meshless.weights = {};
    CHECK(scatter_cell(scatter_generator, meshless, {40, 33}, &*tile, {})->empty()); // no meshes: nothing placed
    CHECK(scatter_spacing_valid(4.0));
    CHECK(scatter_spacing_valid(0.5));
    CHECK_FALSE(scatter_spacing_valid(3.0));
    CHECK_FALSE(scatter_spacing_valid(128.0));
}

TEST_CASE("A scatter generator file holds its kind's properties, its meshes, and its heights, and reads back exactly", "[procedural][generators][scatter]") {
    auto generator = GeneratorAsset{};
    generator.kind = GeneratorKind::scatter;
    generator.version = scatter_version;
    generator.seed = 77;
    generator.heights = AssetRef<GeneratorAsset>{{0x6d617961, 0x90}};
    generator.spacing = 2.0f;
    generator.density = 0.35f;
    generator.max_slope = 22.5f;
    generator.min_height = -3.0f;
    generator.random_yaw = false;
    generator.mesh0 = AssetRef<MeshAsset>{{0x6d617961, 2}};
    generator.material0 = AssetRef<MaterialAsset>{{0x6d617961, 0x11}};
    generator.weight0 = 3.0f;
    generator.mesh2 = AssetRef<MeshAsset>{{0x6d617961, 5}};
    generator.collider2 = 0.75f;
    const auto text = write_generator_file(generator);
    INFO(text);
    CHECK(text.find("heights 6d617961 90\n") != std::string::npos);
    CHECK(text.find("mesh1 none\n") != std::string::npos);
    CHECK(text.find("random_yaw false\n") != std::string::npos);
    CHECK(text.find("octaves") == std::string::npos); // not a scatter property
    auto input = std::istringstream(text);
    const auto read = read_generator_file(input);
    INFO(read.error);
    REQUIRE(read);
    CHECK(read.generator == generator);
    // Its rules: variants without a mesh weigh nothing; the base is the flat ground's height.
    const auto rules = scatter_rules(generator);
    CHECK(rules.weights == std::array{3.0f, 0.0f, 1.0f, 0.0f});
    CHECK(rules.spacing == 2.0);
    CHECK(rules.seed == 77);
    CHECK_FALSE(rules.random_yaw);
    // A heights generator's file is as before: none of scatter's properties.
    CHECK(write_generator_file(GeneratorAsset{}).find("spacing") == std::string::npos);
}
