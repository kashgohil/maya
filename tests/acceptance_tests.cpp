// Milestone acceptance (#1005): a scene authored in the editor, saved, reopened, and run through the
// player's path; the V1 visual reference; and steady resource use across resizes, reloads, and play
// resets. The authored project is written to MAYA_ACCEPTANCE_DIR so the real maya_player can run it
// afterwards (CTest runs it from another working directory).

#include "editor_harness.hpp"
#include "maya/assets/project.hpp"
#include "maya/assets/property_context.hpp"
#include "support/metal_view.hpp"
#include <catch2/catch_approx.hpp>
#include <cstdlib>
#include <sstream>

using namespace maya;
using namespace maya::editor;
using namespace maya::editor::testing;
using namespace maya::test;
using Catch::Approx;

namespace {
const auto cube_mesh = AssetId{0x6d617961, 2};
const auto pyramid_mesh = AssetId{0x6d617961, 1};
const auto red = AssetId{0x6d617961, 0x11};
const auto blue = AssetId{0x6d617961, 0x12};
const auto amber = AssetId{0x6d617961, 0x10};

fs::path acceptance_dir() { return fs::path(MAYA_ACCEPTANCE_DIR); }
fs::path authored_project() { return acceptance_dir() / "Authored Game"; }
constexpr auto authored_scene = "levels/authored.scene";

ImVec2 control(Harness& harness, std::string_view key) {
    const auto* found = harness.shell.layout().control(key);
    INFO(key);
    REQUIRE(found);
    return {(found->min.x + found->max.x) / 2, (found->min.y + found->max.y) / 2};
}
void type(Harness& harness, const std::string& text) {
    for (const auto c : text) harness.frame({TextEvent{uint32_t(c)}});
}
std::string saved_text(Harness& harness) {
    auto out = std::ostringstream{};
    REQUIRE(write_scene(out, harness.shell.scene()->document(), asset_property_context(*harness.shell.assets())).empty());
    return out.str();
}
std::optional<MeshRendererComponent> renderer_of(const World& world, EntityId id) {
    const auto value = read_component(world, *world.find(id), ComponentId::mesh_renderer);
    return value ? std::optional{std::get<MeshRendererComponent>(*value)} : std::nullopt;
}

std::array<int, 3> at(const RgbImage& image, int x, int y) {
    const auto* p = image.rgb.data() + (size_t(y) * image.width + size_t(x)) * 3;
    return {p[0], p[1], p[2]};
}
math::Quat looking(const math::Vec3& from, const math::Vec3& to) {
    const auto f = (to - from).normalized();
    auto q = math::Quat::from_axis_angle({0, 1, 0}, std::atan2(-f.x, -f.z)) * math::Quat::from_axis_angle({1, 0, 0}, std::asin(f.y));
    q.normalize();
    return q;
}
math::Mat4 pose(const math::Vec3& from, const math::Vec3& to) {
    return local_matrix(TransformComponent{from, looking(from, to), {1, 1, 1}});
}
} // namespace

TEST_CASE("A scene is created, placed, edited, saved, and reopened in the editor without code changes", "[acceptance][author]") {
    auto error = std::error_code{};
    fs::remove_all(acceptance_dir(), error);
    fs::create_directories(authored_project());
    fs::create_directories(acceptance_dir() / "elsewhere"); // where the player is started from
    fs::copy(sample_project().parent_path() / "assets", authored_project() / "assets", fs::copy_options::recursive);
    fs::copy_file(sample_project(), authored_project() / "project.maya");

    auto expected = std::string{};
    {
        Harness harness(false);
        REQUIRE(harness.shell.open_project(authored_project()));
        harness.frames(3);
        chord(harness, {KeyCode::LeftSuper}, KeyCode::N); // create
        auto& scene = *harness.shell.scene();
        REQUIRE(scene.world().size() == 2); // a camera and a light
        harness.frames(2);

        // Place: three instances of two shared meshes, dropped from the Assets panel onto the ground.
        drag(harness, control(harness, "asset.cube.obj"), on_screen(harness, {-1.5f, 0.0f, 0.0f}));
        const auto left = *scene.primary();
        drag(harness, control(harness, "asset.pyramid.obj"), on_screen(harness, {0.5f, 0.0f, 0.0f}));
        const auto middle = *scene.primary();
        drag(harness, control(harness, "asset.cube.obj"), on_screen(harness, {2.5f, 0.0f, -0.5f}));
        const auto right = *scene.primary();
        harness.frames(2);
        // Assign materials: red by dropping it on the left cube, blue on the right cube's hierarchy row.
        drag(harness, control(harness, "asset.materials/red.material"), on_screen(harness, {-1.5f, 0.5f, 0.0f}));
        drag(harness, control(harness, "asset.materials/blue_metal.material"), row_center(harness, right));
        REQUIRE(harness.shell.assign_asset(middle, amber));
        // Edit values: a rotated, nonuniformly scaled pyramid; a spinning left cube.
        auto transform = std::get<TransformComponent>(*read_component(scene.world(), *scene.world().find(middle), ComponentId::transform));
        transform.rotation = math::Quat::from_axis_angle({0, 1, 0}, 0.6f);
        transform.scale = {1.2f, 0.8f, 1.2f};
        transform.translation.y = 0.0f; // the pyramid's base sits at its origin
        REQUIRE(scene.set_component(middle, transform));
        REQUIRE(scene.set_component(left, SpinComponent{{0, 1, 0}, 1.0f}));
        // Duplicate the pyramid and move the copy; then parent the right cube under it.
        scene.select(middle);
        chord(harness, {KeyCode::LeftSuper}, KeyCode::D);
        const auto copy = *scene.primary();
        auto moved = transform;
        moved.translation = {-3.5f, 0.0f, -2.0f}; // left and behind, where nothing covers it from the camera
        REQUIRE(scene.set_component(copy, moved));
        REQUIRE(scene.move(right, copy));
        CHECK(scene.record(right)->parent == copy);
        // Save.
        chord(harness, {KeyCode::LeftSuper}, KeyCode::S);
        harness.frames(2);
        REQUIRE(harness.shell.prompt() == EditorPrompt::save_as);
        type(harness, "levels/authored");
        harness.frame(key(KeyCode::Enter, true));
        harness.frame(key(KeyCode::Enter, false));
        REQUIRE(harness.shell.scene_path() == fs::canonical(authored_project()) / "assets" / authored_scene);
        CHECK_FALSE(scene.dirty());
        expected = saved_text(harness);
        // Every placed object has a distinct transform; two meshes are shared by four instances.
        auto matrices = std::vector<math::Mat4>{};
        for (const auto id : {left, middle, right, copy}) matrices.push_back(*scene.world().world_matrix(*scene.world().find(id)));
        for (size_t i = 0; i < matrices.size(); ++i)
            for (size_t j = i + 1; j < matrices.size(); ++j) CHECK_FALSE(std::ranges::equal(matrices[i].elements, matrices[j].elements));
        CHECK(renderer_of(scene.world(), left)->material.id == red);
        CHECK(renderer_of(scene.world(), right)->material.id == blue);
        CHECK(renderer_of(scene.world(), copy)->mesh.id == pyramid_mesh);
    }
    // Reopen in a fresh editor: IDs, hierarchy, values, and root order round-trip exactly.
    Harness reopened(false);
    REQUIRE(reopened.shell.open_project(authored_project()));
    REQUIRE(reopened.shell.open_scene(authored_scene));
    CHECK(saved_text(reopened) == expected);
    CHECK(reopened.shell.scene()->world().size() == 6);
}

TEST_CASE("The authored scene runs through the player's path and draws each object where it was placed", "[acceptance][gpu]") {
    const auto opened = open_project(authored_project());
    INFO("Run the [acceptance][author] test first; it writes the authored project");
    REQUIRE(opened);
    Gpu gpu;
    auto assets = open_project_assets(opened.project, std::make_unique<FileAssetProvider>(gpu.device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto loaded = load_scene_file(*opened.project.resolve(authored_scene), context);
    REQUIRE(loaded);
    auto started = PlaySession::start(std::move(loaded.document), context, builtin_systems());
    REQUIRE(started);
    auto& session = *started.session;
    for (int i = 0; i < 60; ++i) REQUIRE(session.update(1.0 / 60.0).error.empty()); // one second of play
    const auto& world = session.world();
    const auto camera = world.find(*session.camera());
    REQUIRE(camera);
    const auto view = extract_render_view(world, *camera, 320, 180);
    REQUIRE(view);
    const auto image = gpu.render(world, *assets.registry, *view);

    // Each mesh renderer's origin, lifted to its middle, shows its material's color.
    auto checked = 0;
    world.for_each<TransformComponent, MeshRendererComponent>([&](EntityHandle entity, const TransformComponent&,
                                                                   const MeshRendererComponent& renderer) {
        const auto matrix = *world.world_matrix(entity);
        const auto center = math::Vec4(matrix.elements[12], matrix.elements[13] + 0.35f, matrix.elements[14], 1.0f);
        const auto clip = view->matrices.view_projection * center;
        const auto x = int((clip.x / clip.w * 0.5f + 0.5f) * 320.0f), y = int((0.5f - clip.y / clip.w * 0.5f) * 180.0f);
        INFO("entity at pixel " << x << "," << y);
        REQUIRE(x >= 0);
        REQUIRE(x < 320);
        REQUIRE(y >= 0);
        REQUIRE(y < 180);
        const auto [r, g, b] = at(image, x, y);
        INFO("color " << r << " " << g << " " << b);
        if (renderer.material.id == red) CHECK((r > g + 60 && r > b + 60)); // a 4% white highlight lifts green and blue
        else if (renderer.material.id == blue) CHECK((b > r && b > g));
        else if (renderer.material.id == amber) CHECK((r > b && g > b));
        ++checked;
    });
    CHECK(checked == 4);
    // Four instances, two shared meshes: two vertex/index buffer pairs, whatever the instance count.
    const auto residency = assets.registry->residency();
    CHECK(residency.meshes == 2);
    CHECK(gpu.device.stats().buffers == 4);
    CHECK(gpu.device.stats().frame_draws == 4 + 1); // and the tone-mapping triangle
}

TEST_CASE("The V1 reference scene matches its reference images, and authoring and play views agree", "[visual][gpu]") {
    const auto project = open_project(sample_project());
    REQUIRE(project);
    Gpu gpu;
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(gpu.device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto loaded = load_scene_file(*project.project.resolve("v1_reference.scene"), context);
    REQUIRE(loaded);
    auto authored = instantiate_scene(loaded.document, context);
    REQUIRE(authored);
    auto started = PlaySession::start(loaded.document, context, builtin_systems());
    REQUIRE(started);
    auto& session = *started.session;
    constexpr uint32_t width = 256, height = 144;

    // Authoring and play show the same image at tick 0: one extraction path, one camera.
    const auto camera = *session.camera();
    const auto authored_view = extract_render_view(*authored.world, *authored.world->find(camera), width, height);
    const auto play_view = extract_render_view(session.world(), *session.world().find(camera), width, height);
    REQUIRE(authored_view);
    REQUIRE(play_view);
    CHECK(gpu.render(*authored.world, *assets.registry, *authored_view).rgb == gpu.render(session.world(), *assets.registry, *play_view).rgb);

    // Named views: the fixed 600-tick camera path (the rig turns once), and two fixed poses.
    auto images = std::vector<std::pair<std::string, RgbImage>>{};
    for (int tick = 0; tick <= 600; tick += 150) {
        while (session.clock().tick() < uint64_t(tick)) REQUIRE(session.update(1.0 / 60.0).error.empty());
        const auto view = extract_render_view(session.world(), *session.world().find(camera), width, height);
        REQUIRE(view);
        char name[32];
        std::snprintf(name, sizeof(name), "path-%04d", tick);
        images.emplace_back(name, gpu.render(session.world(), *assets.registry, *view));
    }
    const auto fixed = [&](const char* name, const math::Vec3& from, const math::Vec3& to) {
        auto view = make_render_view(CameraComponent{}, pose(from, to), width, height);
        REQUIRE(view);
        images.emplace_back(name, gpu.render(*authored.world, *assets.registry, *view));
    };
    fixed("overview", {0.0f, 18.0f, 14.0f}, {0.0f, 0.0f, -1.0f});
    fixed("overlap", {-1.2f, 0.9f, 9.0f}, {-2.5f, 0.75f, -4.0f}); // the near cube in front of the far one
    // The path returns to its start after one full turn (up to the rounding of 600 small turns).
    const auto& start = images[0].second.rgb;
    const auto& end = images[4].second.rgb;
    auto moved_pixels = size_t{0};
    for (size_t i = 0; i < start.size(); i += 3)
        for (size_t c = 0; c < 3; ++c)
            if (std::abs(int(start[i + c]) - int(end[i + c])) > 6) { ++moved_pixels; break; }
    CHECK(double(moved_pixels) / double(width * height) <= 0.005);

    // Compare with the blessed references: each channel within 6 on at least 99.5% of pixels.
    compare_with_references(fs::path(MAYA_SOURCE_DIR) / "tests/references/v1", fs::path(MAYA_ACCEPTANCE_DIR).parent_path() / "visual-diffs",
                            images);
}

TEST_CASE("The V1 overview through exposure, both tone mappers, and the exposure views matches its HDR references", "[visual][gpu]") {
    const auto project = open_project(sample_project());
    REQUIRE(project);
    Gpu gpu;
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(gpu.device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto loaded = load_scene_file(*project.project.resolve("v1_reference.scene"), context);
    REQUIRE(loaded);
    auto authored = instantiate_scene(loaded.document, context);
    REQUIRE(authored);
    constexpr uint32_t width = 256, height = 144;
    auto images = std::vector<std::pair<std::string, RgbImage>>{};
    const auto overview = [&](const char* name, float exposure, ToneMapping tone, ExposureView shown = ExposureView::none) {
        auto camera = CameraComponent{};
        camera.exposure = exposure;
        camera.tone_mapping = tone;
        auto view = make_render_view(camera, pose({0.0f, 18.0f, 14.0f}, {0.0f, 0.0f, -1.0f}), width, height);
        REQUIRE(view);
        view->exposure_view = shown;
        images.emplace_back(name, gpu.render(*authored.world, *assets.registry, *view));
    };
    overview("ev-minus2", -2.0f, ToneMapping::agx); // brighter: the lit floor rolls off toward white without clipping
    overview("ev-plus2", 2.0f, ToneMapping::agx);
    overview("pbr-neutral", 0.0f, ToneMapping::pbr_neutral);
    overview("luminance", 0.0f, ToneMapping::agx, ExposureView::luminance);
    overview("false-color", 0.0f, ToneMapping::agx, ExposureView::false_color);
    // Four stops of exposure apart, the brighter image is brighter on average.
    const auto mean = [&](const RgbImage& image) {
        auto total = 0.0;
        for (const auto value : image.rgb) total += value;
        return total / double(image.rgb.size());
    };
    CHECK(mean(images[0].second) > mean(images[1].second) + 40.0);
    compare_with_references(fs::path(MAYA_SOURCE_DIR) / "tests/references/hdr", fs::path(MAYA_ACCEPTANCE_DIR).parent_path() / "visual-diffs",
                            images);
}

TEST_CASE("The material test scene matches its references: spheres across metallic and roughness, and textured surfaces", "[visual][gpu]") {
    const auto project = open_project(sample_project());
    REQUIRE(project);
    Gpu gpu;
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(gpu.device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto loaded = load_scene_file(*project.project.resolve("materials.scene"), context);
    REQUIRE(loaded);
    auto authored = instantiate_scene(loaded.document, context);
    REQUIRE(authored);
    constexpr uint32_t width = 512, height = 288;
    auto images = std::vector<std::pair<std::string, RgbImage>>{};
    const auto camera = authored.world->find(EntityId{0x6d617961, 0x500});
    REQUIRE(camera);
    auto view = extract_render_view(*authored.world, *camera, width, height);
    REQUIRE(view);
    images.emplace_back("overview", gpu.render(*authored.world, *assets.registry, *view));
    view->tone_mapping = ToneMapping::pbr_neutral;
    images.emplace_back("pbr-neutral", gpu.render(*authored.world, *assets.registry, *view));
    // The textured row from close by, and from behind so the cutout's back faces and the glass show.
    const auto close = [&](const char* name, const math::Vec3& from, const math::Vec3& to) {
        auto lens = CameraComponent{};
        lens.vertical_fov = 0.6f;
        auto near = make_render_view(lens, pose(from, to), width, height);
        REQUIRE(near);
        images.emplace_back(name, gpu.render(*authored.world, *assets.registry, *near));
    };
    close("textured", {0.0f, 0.6f, 7.0f}, {0.0f, -0.5f, 0.0f});
    close("textured-behind", {-2.0f, 0.8f, -6.0f}, {0.6f, -0.5f, 0.0f});
    compare_with_references(fs::path(MAYA_SOURCE_DIR) / "tests/references/materials",
                            fs::path(MAYA_ACCEPTANCE_DIR).parent_path() / "visual-diffs", images);
}

TEST_CASE("Resizing, reloading, and play resets settle at a steady state without stale handles", "[acceptance][gpu]") {
    Gpu gpu;
    const auto project = open_project(sample_project());
    REQUIRE(project);
    auto assets = open_project_assets(project.project, std::make_unique<FileAssetProvider>(gpu.device));
    REQUIRE(assets);
    const auto context = asset_property_context(*assets.registry);
    auto& registry = *assets.registry;

    // Resizing: one color, one HDR scene color, and one depth texture at the current size, whatever came before.
    const auto sizes = std::array<std::pair<uint32_t, uint32_t>, 5>{{{320, 180}, {640, 360}, {97, 31}, {1280, 720}, {320, 180}}};
    auto old_color = TextureHandle{};
    std::optional<size_t> reported_after_first;
    for (int round = 0; round < 40; ++round) {
        for (const auto [w, h] : sizes) {
            REQUIRE_FALSE(gpu.target->resize(w, h));
            REQUIRE_FALSE(gpu.device.begin_frame());
            REQUIRE_FALSE(gpu.device.end_frame());
        }
        if (round == 0) {
            old_color = gpu.target->color();
            gpu.device.wait_idle();
            reported_after_first = gpu.device.reported_memory();
        }
    }
    gpu.device.wait_idle();
    auto stats = gpu.device.stats();
    CHECK(stats.textures == 3); // color, HDR scene color, and depth
    CHECK(stats.texture_bytes == size_t{320} * 180 * (4 + 8 + 4));
    CHECK(stats.pending_retirements == 0);
    CHECK(gpu.device.describe(old_color) == nullptr); // the first round's texture is gone for good
    REQUIRE(reported_after_first);
    CHECK(*gpu.device.reported_memory() <= *reported_after_first + (size_t{32} << 20)); // no growth beyond noise

    // Reloading: a released version is evicted, its old handle stays stale, and a reload is a new version.
    {
        auto lease = registry.acquire(AssetRef<MeshAsset>{cube_mesh});
        REQUIRE(lease);
        const auto old_handle = lease.lease.handle();
        lease = {};
        CHECK(registry.evict_unused() == 1);
        CHECK(registry.resolve(old_handle).diagnostic.code == AssetError::stale_handle);
        const auto again = registry.acquire(AssetRef<MeshAsset>{cube_mesh});
        REQUIRE(again);
        CHECK(again.lease.handle().generation != old_handle.generation);
    }

    // Play resets: each session is a new World; handles from one never resolve in the next.
    auto loaded = load_scene_file(*project.project.startup_scene, context);
    REQUIRE(loaded);
    auto previous = std::optional<EntityHandle>{};
    for (int round = 0; round < 25; ++round) {
        auto started = PlaySession::start(loaded.document, context, builtin_systems());
        REQUIRE(started);
        auto& world = started.session->world();
        if (previous) CHECK_FALSE(world.alive(*previous));
        previous = *world.find(*started.session->camera());
        for (int tick = 0; tick < 5; ++tick) REQUIRE(started.session->update(1.0 / 60.0).error.empty());
        const auto view = extract_render_view(world, *previous, 320, 180);
        REQUIRE(view);
        gpu.render(world, registry, *view);
    }
    gpu.device.wait_idle();
    registry.evict_unused();
    stats = gpu.device.stats();
    CHECK(stats.pending_retirements == 0);
    CHECK(registry.residency().leased == 0);
    CHECK(stats.buffers == 2 * registry.residency().meshes); // only resident meshes hold buffers
}
