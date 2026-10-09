#include "maya/renderer/renderer.hpp"
#include "maya/renderer/shader_constants.hpp"
#include "maya/rhi/null_device.hpp"
#include "support/hdr.hpp"
#include "support/render_scene.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <set>
#include <unordered_map>

using namespace maya;
using namespace maya::test;
using Catch::Approx;

namespace {
/// Null backend that mirrors buffer contents and records the constants bound at each draw.
class CapturingDevice final : public NullGraphicsDevice {
public:
    struct Draw {
        DrawConstants constants;
        MaterialConstants material; // as bound at the draw, whichever draw uploaded it
        ViewConstants view;
        uint32_t vertex_buffer;
        uint32_t index_buffer;
        std::string pipeline; // its label
        std::array<uint32_t, material_slots> textures; // bound per MaterialSlot, by native slot
    };
    struct Present {
        PresentConstants constants;
        uint32_t texture;
    };
    struct ToneMap {
        ToneMapConstants constants;
        uint32_t texture; // the scene color it read
    };
    struct DebugCall {
        std::string pipeline; // its label: in front of the scene, or behind it
        DebugConstants constants;
        uint32_t vertices, instances;
        std::vector<float> data; // what buffer 0 holds for this draw
    };
    explicit CapturingDevice(DeviceOptions options = {}) : NullGraphicsDevice({false, 0, 0, true}) {
        REQUIRE(initialize(nullptr, options));
    }
    ~CapturingDevice() override { shutdown(); }
    /// Frames complete only when told to, so retirement is observable.
    void finish_frames() { complete_through(stats().submitted_frames); }

    struct ShadowDraw {
        ShadowConstants shadow;
        DrawConstants constants;
        std::string pipeline;
    };
    struct Call {
        std::string pipeline;
        uint32_t instances;
    };
    std::vector<Draw> draws; // one per instance drawn in a view
    std::vector<Call> draw_calls, shadow_calls; // the instanced draws themselves
    std::vector<ShadowDraw> shadow_draws; // into shadow maps, apart from the view's draws
    std::vector<RenderPassDesc> shadow_passes; // shadow maps, and the one-time clear of the empty one
    std::vector<Present> presents;
    std::vector<ToneMap> tone_maps;
    std::vector<DebugCall> debug;
    std::vector<RenderPassDesc> passes;
    size_t pipelines_created = 0;
    std::vector<std::string> pipeline_labels; // in creation order
    size_t debug_sources = 0; // pipelines compiled with the debug views' define
    size_t material_binds = 0; // material constants bound (buffer 3)
    std::vector<std::string> sequence; // the pipeline of every draw, in order
    std::unordered_map<uint32_t, uint32_t> bound_textures; // slot to native texture, as last bound

protected:
    RhiDiagnostic backend_create_buffer(uint32_t slot, const BufferDesc& desc, const void* data) override {
        auto& bytes = m_buffers[slot];
        bytes.assign(desc.size, std::byte{});
        if (data) std::memcpy(bytes.data(), data, desc.size);
        return NullGraphicsDevice::backend_create_buffer(slot, desc, data);
    }
    void backend_write_buffer(uint32_t slot, size_t offset, const void* data, size_t size) noexcept override {
        std::memcpy(m_buffers[slot].data() + offset, data, size);
    }
    RhiDiagnostic backend_begin_pass(const RenderPassDesc& desc) override {
        (desc.label.ends_with("shadows") ? shadow_passes : passes).push_back(desc);
        return NullGraphicsDevice::backend_begin_pass(desc);
    }
    void backend_set_vertex_buffer(uint32_t index, uint32_t slot, size_t offset) override { m_bound[index] = {slot, offset}; }
    void backend_set_uniform_buffer(uint32_t index, uint32_t slot, size_t offset) override {
        m_bound[index] = {slot, offset};
        if (index == 3) ++material_binds;
    }
    void backend_set_texture(uint32_t index, uint32_t slot) override {
        m_texture = slot;
        bound_textures[index] = slot;
        if (index < material_slots) m_textures[index] = slot;
    }
    RhiDiagnostic backend_create_pipeline(uint32_t slot, const PipelineDesc& desc) override {
        m_labels[slot] = desc.label;
        ++pipelines_created;
        pipeline_labels.push_back(desc.label);
        if (desc.shader_source.starts_with("#define MAYA_DEBUG_VIEWS 1\n")) ++debug_sources;
        return NullGraphicsDevice::backend_create_pipeline(slot, desc);
    }
    void backend_set_pipeline(uint32_t slot) override {
        m_pipeline = m_labels[slot];
        NullGraphicsDevice::backend_set_pipeline(slot);
    }
    void backend_draw(uint32_t vertices, uint32_t, uint32_t instances) override {
        sequence.push_back(m_pipeline);
        if (m_pipeline == "sky") return;
        if (m_pipeline == "tone map") {
            tone_maps.push_back({read<ToneMapConstants>(0), m_texture});
            return;
        }
        if (!m_pipeline.starts_with("debug")) { // the present pipeline
            presents.push_back({read<PresentConstants>(1), m_texture});
            return;
        }
        const auto constants = read<DebugConstants>(1);
        const auto& [slot, offset] = m_bound.at(0);
        const auto floats = constants.kind[0] == 0 ? size_t(vertices / 6) * debug_line_floats : size_t(instances) * debug_shape_floats;
        auto data = std::vector<float>(floats);
        std::memcpy(data.data(), m_buffers.at(slot).data() + offset, floats * sizeof(float));
        debug.push_back({m_pipeline, constants, vertices, instances, std::move(data)});
    }
    // Instanced draws (#1025) are recorded as one draw per instance, each with the transform its instance
    // number picks through the pass's order (buffer 4) from the view's instances (buffer 1).
    void backend_draw_indexed(uint32_t slot, IndexType, uint32_t, size_t, uint32_t instances, uint32_t first) override {
        const auto shadow = m_pipeline.ends_with("shadow caster");
        (shadow ? shadow_calls : draw_calls).push_back({m_pipeline, instances});
        for (uint32_t k = first; k < first + instances; ++k) {
            const auto index = read<uint32_t>(4, k * sizeof(uint32_t));
            const auto constants = read<DrawConstants>(1, index * sizeof(DrawConstants));
            if (shadow) {
                shadow_draws.push_back({read<ShadowConstants>(2), constants, m_pipeline});
                continue;
            }
            sequence.push_back(m_pipeline);
            draws.push_back({constants, read<MaterialConstants>(3), read<ViewConstants>(2), m_bound.at(0).first, slot, m_pipeline, m_textures});
        }
    }

private:
    template<class T> T read(uint32_t index, size_t at = 0) const {
        const auto& [slot, offset] = m_bound.at(index);
        auto value = T{};
        std::memcpy(&value, m_buffers.at(slot).data() + offset + at, sizeof(T));
        return value;
    }
    std::unordered_map<uint32_t, std::vector<std::byte>> m_buffers;
    std::unordered_map<uint32_t, std::pair<uint32_t, size_t>> m_bound;
    std::unordered_map<uint32_t, std::string> m_labels;
    std::string m_pipeline;
    uint32_t m_texture = 0;
    std::array<uint32_t, material_slots> m_textures{};
};

constexpr auto camera_component = CameraComponent{};
const auto red = MaterialAsset{{1, 0, 0, 1}, 0.25f, 0.5f};
const auto blue = MaterialAsset{{0, 0, 1, 1}, 1.0f, 0.2f};

math::Vec3 transform_point(const math::Mat4& m, const math::Vec3& p) {
    const auto v = m * math::Vec4(p, 1.0f);
    return {v.x, v.y, v.z};
}
math::Vec3 transform_direction(const math::Mat4& m, const math::Vec3& d) {
    const auto v = m * math::Vec4(d, 0.0f);
    return {v.x, v.y, v.z};
}
math::Vec3 apply(const std::array<math::Vec3, 3>& columns, const math::Vec3& n) {
    return columns[0] * n.x + columns[1] * n.y + columns[2] * n.z;
}
bool same(const math::Mat4& a, const math::Mat4& b) {
    return std::memcmp(a.elements, b.elements, sizeof(a.elements)) == 0;
}
RenderView view_of(uint32_t width, uint32_t height) {
    const auto view = make_render_view(camera_component, look_pose({0, 0, 5}, {0, 0, 0}), width, height);
    REQUIRE(view);
    return *view;
}
/// A view of `target` from 10 m along +Z, so instances there are not culled.
RenderView view_toward(const math::Vec3& target, uint32_t width = 8, uint32_t height = 8) {
    const auto view = make_render_view(camera_component, look_pose(target + math::Vec3{0, 0, 10}, target), width, height);
    REQUIRE(view);
    return *view;
}
} // namespace

TEST_CASE("Extraction shares one mesh lease across instances and copies transforms and materials", "[renderer]") {
    CapturingDevice device;
    TestProject project(device, {{"cube.mesh", unit_cube()}}, {{"red.material", red}, {"blue.material", blue}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    const auto red_ref = project.add<MaterialAsset>(2, "red.material");
    const auto blue_ref = project.add<MaterialAsset>(3, "blue.material");
    World world;
    const auto created = build_world(world, [&](WorldCommands& commands) {
        const std::pair<float, AssetRef<MaterialAsset>> placements[] = {{-2.0f, red_ref}, {0.0f, blue_ref}, {2.0f, {}}};
        for (const auto& [x, material] : placements) {
            auto entity = commands.create();
            commands.add(entity, TransformComponent{{x, 0, 0}, {}, {1.0f}});
            commands.add(entity, MeshRendererComponent{cube, material, true});
        }
    });

    const auto snapshot = extract_render_snapshot(world, *project.registry);
    CHECK(snapshot.world == world.token());
    REQUIRE(snapshot.meshes.size() == 1);
    REQUIRE(snapshot.instances.size() == 3);
    CHECK(snapshot.diagnostics.empty());
    CHECK(*project.loads == 3); // one mesh load and two material loads for three instances
    for (const auto& instance : snapshot.instances) CHECK(instance.mesh == 0);
    const auto instance_of = [&](EntityHandle entity) {
        const auto id = *world.persistent_id(entity);
        const auto found = std::ranges::find(snapshot.instances, id, &RenderInstance::entity);
        REQUIRE(found != snapshot.instances.end());
        return *found;
    };
    CHECK(same(instance_of(created[0]).world, math::Mat4::translate({-2, 0, 0})));
    CHECK(same(instance_of(created[2]).world, math::Mat4::translate({2, 0, 0})));
    CHECK(snapshot.materials[instance_of(created[0]).material].base_color.x == 1.0f);
    CHECK(snapshot.materials[instance_of(created[0]).material].metallic == 0.25f);
    CHECK(snapshot.materials[instance_of(created[1]).material].base_color.z == 1.0f);
    CHECK(snapshot.materials[instance_of(created[1]).material].roughness == 0.2f);
    const auto unassigned = MaterialAsset{};
    CHECK(snapshot.materials[instance_of(created[2]).material].base_color.y == unassigned.base_color.y);
    CHECK(snapshot.materials[instance_of(created[2]).material].roughness == unassigned.roughness);

    // Every instance draws the one shared mesh with its own constants.
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(32, 32));
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(renderer.render(snapshot, view_of(32, 32), target));
    REQUIRE_FALSE(device.end_frame());
    REQUIRE(device.draws.size() == 3);
    const auto& mesh = snapshot.meshes[0].value().mesh();
    for (size_t i = 0; i < 3; ++i) {
        CHECK(device.draws[i].vertex_buffer == mesh.vertex_buffer().slot);
        CHECK(device.draws[i].index_buffer == mesh.index_buffer().slot);
        CHECK(same(device.draws[i].constants.model, snapshot.instances[i].world));
        CHECK(device.draws[i].material.base_color.x == snapshot.materials[snapshot.instances[i].material].base_color.x);
        CHECK(device.draws[i].material.factors.x == snapshot.materials[snapshot.instances[i].material].metallic);
        CHECK(device.draws[i].material.factors.y == snapshot.materials[snapshot.instances[i].material].roughness);
    }
    CHECK(renderer.stats().draws == 3);
}

TEST_CASE("Extraction skips missing meshes, substitutes failed materials, and reports each", "[renderer]") {
    CapturingDevice device;
    TestProject project(device, {{"cube.mesh", unit_cube()}}, {{"red.material", red}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    const auto gone = project.add<MeshAsset>(2, "gone.mesh"); // registered, but the file is missing
    const auto broken = project.add<MaterialAsset>(3, "broken.material"); // provider has no such material
    const auto unregistered = AssetRef<MeshAsset>{{0x7465, 99}};
    project.touch("broken.material");
    World world;
    const auto created = build_world(world, [&](WorldCommands& commands) {
        const auto add = [&](MeshRendererComponent renderer, bool transform = true) {
            auto entity = commands.create();
            if (transform) commands.add(entity, TransformComponent{});
            commands.add(entity, renderer);
        };
        add({cube, {}, true}); // drawn
        add({cube, broken, true}); // drawn with the fallback material
        add({gone, {}, true}); // missing file
        add({gone, {}, true}); // same missing mesh: reported, not reloaded
        add({unregistered, {}, true}); // not in the catalog
        add({cube, {}, false}); // hidden
        add({{}, {}, true}); // no mesh assigned
        add({cube, {}, true}, false); // no transform
    });

    const auto snapshot = extract_render_snapshot(world, *project.registry);
    CHECK(snapshot.stats.mesh_renderers == 8);
    CHECK(snapshot.stats.hidden == 2);
    CHECK(snapshot.stats.skipped == 4);
    REQUIRE(snapshot.instances.size() == 2);
    CHECK(snapshot.meshes.size() == 1);
    const auto count = [&](RenderIssue code) {
        return std::ranges::count(snapshot.diagnostics, code, &RenderDiagnostic::code);
    };
    CHECK(count(RenderIssue::missing_mesh) == 3);
    CHECK(count(RenderIssue::missing_material) == 1);
    CHECK(count(RenderIssue::missing_transform) == 1);
    for (const auto& diagnostic : snapshot.diagnostics) {
        CHECK(diagnostic.entity.valid());
        CHECK_FALSE(diagnostic.message.empty());
    }
    const auto fallback = std::ranges::find(snapshot.instances, *world.persistent_id(created[1]), &RenderInstance::entity);
    REQUIRE(fallback != snapshot.instances.end());
    CHECK(snapshot.materials[fallback->material].base_color.x == fallback_material().base_color.x);
    CHECK(snapshot.materials[fallback->material].base_color.y == fallback_material().base_color.y);
    const auto missing = std::ranges::find(snapshot.diagnostics, gone.id, &RenderDiagnostic::asset);
    REQUIRE(missing != snapshot.diagnostics.end());
    CHECK(missing->message.find("gone.mesh") != std::string::npos);
    // Failed entries are not retried on every extraction.
    const auto loads = *project.loads;
    CHECK(extract_render_snapshot(world, *project.registry).instances.size() == 2);
    CHECK(*project.loads == loads);
}

TEST_CASE("Normal matrices keep lighting normals perpendicular under nonuniform scale and hierarchy", "[renderer]") {
    CapturingDevice device;
    TestProject project(device, {{"cube.mesh", unit_cube()}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    World world;
    const auto created = build_world(world, [&](WorldCommands& commands) {
        auto parent = commands.create();
        commands.add(parent, TransformComponent{{1, 2, 3}, math::Quat::from_axis_angle({0, 0, 1}, 0.7f), {1, 4, 1}});
        auto child = commands.create();
        commands.add(child, TransformComponent{{0, 1, 0}, math::Quat::from_axis_angle({1, 0, 0}, 0.4f), {2, 1, 0.5f}});
        commands.add(child, MeshRendererComponent{cube, {}, true});
        commands.reparent(child, parent, ReparentPolicy::keep_local);
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    REQUIRE(snapshot.instances.size() == 1);
    const auto& instance = snapshot.instances[0];
    CHECK(same(instance.world, world.world_matrix(created[1])->matrix()));
    // A slanted surface: normal n with tangents t1, t2 in object space.
    const auto n = math::Vec3(0.0f, 1.0f, 1.0f).normalized();
    const auto t1 = math::Vec3(1.0f, 0.0f, 0.0f), t2 = math::Vec3(0.0f, 1.0f, -1.0f).normalized();
    const auto world_normal = apply(instance.normal_matrix, n).normalized();
    for (const auto& t : {t1, t2})
        CHECK(math::Vec3::dot(world_normal, transform_direction(instance.world, t).normalized()) == Approx(0.0f).margin(1e-5));
    // The model matrix alone would tilt the normal off the surface.
    const auto naive = transform_direction(instance.world, n).normalized();
    CHECK(std::abs(math::Vec3::dot(naive, transform_direction(instance.world, t2).normalized())) > 0.1f);
    // Outward orientation is preserved: the normal points to the side the surface faces.
    CHECK(math::Vec3::dot(world_normal, transform_direction(instance.world, n)) > 0.0f);
    CHECK(transform_point(instance.world, {0, 0, 0}).y > 2.0f);

    // The same world data reaches the draw constants.
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(8, 8));
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(renderer.render(snapshot, view_toward(transform_point(instance.world, {0, 0, 0})), target));
    REQUIRE_FALSE(device.end_frame());
    REQUIRE(device.draws.size() == 1);
    for (size_t c = 0; c < 3; ++c) {
        CHECK(device.draws[0].constants.normal_matrix[c].x == instance.normal_matrix[c].x);
        CHECK(device.draws[0].constants.normal_matrix[c].z == instance.normal_matrix[c].z);
    }
}

TEST_CASE("Directional lights come from entity rotation, color, and intensity", "[renderer]") {
    CapturingDevice device;
    TestProject project(device, {});
    World world;
    const auto tilt = math::Quat::from_axis_angle({1, 0, 0}, -math::PI / 2.0f); // local +Z to world +Y
    const auto created = build_world(world, [&](WorldCommands& commands) {
        const auto light = [&](EntityId id, LightComponent value, bool transform = true) {
            auto entity = commands.create(id);
            if (transform) commands.add(entity, TransformComponent{{0, 0, 0}, tilt, {1.0f}});
            commands.add(entity, value);
        };
        light({0, 5}, {LightKind::directional, {1.0f, 0.5f, 0.25f}, 2.0f});
        for (uint64_t i = 1; i <= 4; ++i) light({0, 10 + i}, {});
        light({0, 1}, {LightKind::directional, {1.0f}, 1.0f, 10, 0.5f, 0.7f, false}); // disabled
        light({0, 2}, {LightKind::point}); // a local light
        light({0, 3}, {}, false); // no transform
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry, {{0.5f, 0.25f, 0.125f}});
    REQUIRE(snapshot.lights.size() == max_directional_lights);
    CHECK(snapshot.lights[0].entity == EntityId{0, 5}); // selected in EntityId order
    CHECK(snapshot.lights[3].entity == EntityId{0, 13});
    CHECK(snapshot.lights[0].direction_to_light.y == Approx(1.0f));
    CHECK(snapshot.lights[0].direction_to_light.z == Approx(0.0f).margin(1e-6));
    CHECK(snapshot.lights[0].radiance.x == 2.0f);
    CHECK(snapshot.lights[0].radiance.z == 0.5f);
    const auto count = [&](RenderIssue code) {
        return std::ranges::count(snapshot.diagnostics, code, &RenderDiagnostic::code);
    };
    CHECK(count(RenderIssue::light_limit) == 1);
    REQUIRE(snapshot.local_lights.size() == 1);
    CHECK(snapshot.local_lights[0].entity == EntityId{0, 2});
    CHECK(count(RenderIssue::missing_transform) == 1);
    CHECK(snapshot.ambient.y == 0.25f);
    (void)created;

    // Light and ambient values reach the per-view constants.
    auto one = RenderSnapshot{};
    one.world = snapshot.world;
    one.lights = {snapshot.lights[0]};
    one.ambient = snapshot.ambient;
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(4, 4));
    TestProject cubes(device, {{"cube.mesh", unit_cube()}});
    auto with_mesh = one;
    with_mesh.meshes.push_back(cubes.registry->acquire(cubes.add<MeshAsset>(1, "cube.mesh")).lease);
    with_mesh.materials.push_back({});
    with_mesh.instances.push_back({});
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(renderer.render(with_mesh, view_of(4, 4), target));
    REQUIRE_FALSE(device.end_frame());
    REQUIRE(device.draws.size() == 1);
    const auto& view = device.draws[0].view;
    CHECK(view.light_count[0] == 1);
    CHECK(view.lights[0].direction_to_light.y == Approx(1.0f));
    CHECK(view.lights[0].radiance.x == 2.0f);
    CHECK(view.ambient.x == 0.5f);
    CHECK(view.camera_position.z == Approx(5.0f));
}

TEST_CASE("Snapshots own their meshes after entity deletion, eviction, and registry destruction", "[renderer]") {
    CapturingDevice device;
    auto project = std::make_unique<TestProject>(device, std::map<std::string, Geometry>{{"cube.mesh", unit_cube()}});
    const auto cube = project->add<MeshAsset>(1, "cube.mesh");
    auto world = std::make_unique<World>();
    const auto created = build_world(*world, [&](WorldCommands& commands) {
        for (int i = 0; i < 2; ++i) {
            auto entity = commands.create();
            commands.add(entity, TransformComponent{{float(i), 0, 0}, {}, {1.0f}});
            commands.add(entity, MeshRendererComponent{cube, {}, true});
        }
    });
    auto snapshot = std::make_optional(extract_render_snapshot(*world, *project->registry));
    REQUIRE(snapshot->instances.size() == 2);

    // Delete an entity, evict, then destroy the registry and the World: the snapshot is unaffected.
    auto commands = world->commands();
    commands.destroy(created[0]);
    REQUIRE(world->commit(commands));
    CHECK(project->registry->evict_unused() == 0); // the snapshot still leases the mesh
    CHECK(extract_render_snapshot(*world, *project->registry).instances.size() == 1);
    world.reset();
    project.reset();
    REQUIRE(snapshot->meshes[0].value().mesh().valid());

    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(8, 8));
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(renderer.render(*snapshot, view_of(8, 8), target));
    const auto encoded = device.native_resources();
    // Releasing the snapshot inside the frame revokes the mesh handles immediately...
    snapshot.reset();
    CHECK(device.stats().buffers == 0);
    CHECK(device.stats().pending_retirements == 2);
    REQUIRE_FALSE(device.end_frame());
    // ...but the native buffers stay until the GPU finishes the frame that drew them.
    CHECK(device.native_resources() == encoded);
    device.finish_frames();
    REQUIRE_FALSE(device.begin_frame()); // collects completed retirements
    REQUIRE_FALSE(device.end_frame());
    CHECK(device.native_resources() == encoded - 2);
    CHECK(device.stats().pending_retirements == 0);
    CHECK(device.draws.size() == 2);
}

TEST_CASE("One snapshot renders several views of different sizes without another World", "[renderer]") {
    CapturingDevice device;
    TestProject project(device, {{"cube.mesh", unit_cube()}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    World world;
    const auto created = build_world(world, [&](WorldCommands& commands) {
        auto camera = commands.create();
        commands.add(camera, look_transform({0, 0, 6}, {0, 0, 0}));
        commands.add(camera, CameraComponent{});
        for (int i = 0; i < 3; ++i) {
            auto entity = commands.create();
            commands.add(entity, TransformComponent{{float(i) - 1.0f, 0, 0}, {}, {0.5f}});
            commands.add(entity, MeshRendererComponent{cube, {}, true});
        }
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    const auto player = extract_render_view(world, created[0], 64, 64);
    const auto editor = make_render_view(CameraComponent{0.5f, 0.1f, 50.0f}, look_pose({4, 3, 4}, {0, 0, 0}), 40, 20);
    REQUIRE(player);
    REQUIRE(editor);
    auto renderer = Renderer(device, "test source");
    auto player_target = RenderTarget(device, {Format::rgba8_unorm, false, "player"});
    auto editor_target = RenderTarget(device, {Format::rgba8_unorm, false, "editor"});
    REQUIRE_FALSE(player_target.resize(64, 64));
    REQUIRE_FALSE(editor_target.resize(40, 20));
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(renderer.render(snapshot, *player, player_target));
    REQUIRE_FALSE(renderer.render(snapshot, *editor, editor_target));
    REQUIRE_FALSE(device.end_frame());

    CHECK(renderer.stats().views == 2);
    REQUIRE(device.draws.size() == 6);
    // Each view: the scene into its HDR color and depth, then tone mapping into its color.
    REQUIRE(device.passes.size() == 4);
    CHECK(device.passes[0].colors[0].texture == player_target.scene_color());
    CHECK(device.passes[0].label == "view");
    CHECK(device.passes[1].colors[0].texture == player_target.color());
    CHECK(device.passes[1].label == "tone map");
    CHECK_FALSE(device.passes[1].depth);
    CHECK(device.passes[2].colors[0].texture == editor_target.scene_color());
    CHECK(device.passes[2].depth->texture == editor_target.depth());
    CHECK(device.passes[2].depth->store == StoreAction::dont_care); // no debug lines need it
    CHECK(device.passes[3].colors[0].texture == editor_target.color());
    REQUIRE(device.tone_maps.size() == 2);
    CHECK(device.tone_maps[0].texture == player_target.scene_color().slot);
    CHECK(device.tone_maps[1].texture == editor_target.scene_color().slot);
    CHECK(device.tone_maps[0].constants.exposure == Approx(exposure_scale(0.0f)));
    CHECK(device.tone_maps[0].constants.tone_mapping == uint32_t(ToneMapping::agx));
    // Each in the snapshot's frame (#1065): the camera at its offset from the snapshot's origin.
    CHECK(same(device.draws[0].view.view_projection, view_frame(snapshot, *player).matrices.view_projection));
    CHECK(same(device.draws[3].view.view_projection, view_frame(snapshot, *editor).matrices.view_projection));
    for (size_t i = 0; i < 3; ++i) CHECK(same(device.draws[i].constants.model, device.draws[i + 3].constants.model));
    // Aspect comes from each view's own size.
    CHECK(player->matrices.projection.at(0, 0) == Approx(player->matrices.projection.at(1, 1)));
    CHECK(editor->matrices.projection.at(0, 0) == Approx(editor->matrices.projection.at(1, 1) / 2.0f));
    CHECK(player->position.z == Approx(6.0f));
}

TEST_CASE("Views require a camera, a nonzero size, and a rigid pose", "[renderer]") {
    World world;
    const auto created = build_world(world, [&](WorldCommands& commands) {
        auto camera = commands.create();
        commands.add(camera, TransformComponent{});
        commands.add(camera, CameraComponent{});
        auto scaled = commands.create();
        commands.add(scaled, TransformComponent{{}, {}, {2.0f}});
        commands.add(scaled, CameraComponent{});
        auto plain = commands.create();
        commands.add(plain, TransformComponent{});
    });
    CHECK(extract_render_view(world, created[0], 16, 9));
    CHECK_FALSE(extract_render_view(world, created[0], 0, 9));
    CHECK_FALSE(extract_render_view(world, created[0], 16, 0));
    CHECK_FALSE(extract_render_view(world, created[1], 16, 9)); // scaled camera
    CHECK_FALSE(extract_render_view(world, created[2], 16, 9)); // not a camera
    CHECK_FALSE(extract_render_view(world, EntityHandle{}, 16, 9));
    CHECK_FALSE(make_render_view(CameraComponent{}, math::Mat4::scale({1, 2, 1}), 16, 9));
    CHECK_FALSE(make_render_view(CameraComponent{0.0f}, math::Mat4::identity(), 16, 9));
    const auto view = make_render_view(CameraComponent{}, math::Mat4::translate({1, 2, 3}), 16, 9);
    REQUIRE(view);
    CHECK(view->width == 16);
    CHECK(view->position.y == 2.0f);
}

TEST_CASE("Render targets reallocate only on resize and retire replaced textures after completion", "[renderer]") {
    CapturingDevice device;
    auto target = RenderTarget(device, {Format::rgba8_unorm, true, "view"});
    CHECK_FALSE(target.valid());
    CHECK(target.resize(0, 4).code == RhiError::invalid_descriptor);
    REQUIRE_FALSE(target.resize(32, 16));
    CHECK(target.valid());
    CHECK(target.allocations() == 1);
    const auto* color = device.describe(target.color());
    REQUIRE(color);
    CHECK(has_flag(color->usage, TextureUsage::sampled | TextureUsage::render_target | TextureUsage::readback));
    CHECK(device.describe(target.depth())->format == Format::depth32_float);

    // Steady frames at one size never reallocate.
    for (int frame = 0; frame < 5; ++frame) {
        REQUIRE_FALSE(device.begin_frame());
        REQUIRE_FALSE(target.resize(32, 16));
        REQUIRE_FALSE(device.end_frame());
        device.finish_frames();
    }
    CHECK(target.allocations() == 1);

    const auto textures = device.stats().textures;
    const auto old_color = target.color();
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(target.resize(20, 40));
    REQUIRE_FALSE(device.end_frame());
    CHECK(target.allocations() == 2);
    CHECK(target.width() == 20);
    CHECK(target.height() == 40);
    CHECK_FALSE(device.describe(old_color));
    CHECK(device.stats().textures == textures);
    CHECK(device.stats().pending_retirements == 3); // color, scene color, and depth
    device.finish_frames();
    device.wait_idle();
    CHECK(device.stats().pending_retirements == 0);

    // A failed resize keeps the previous textures.
    CHECK(target.resize(device.limits().max_texture_dimension + 1, 4));
    CHECK(target.valid());
    CHECK(target.width() == 20);

    // A new device session invalidates the target until it is resized again.
    device.shutdown();
    CHECK_FALSE(target.valid());
    REQUIRE(device.initialize(nullptr));
    REQUIRE_FALSE(target.resize(20, 40));
    CHECK(target.valid());
    CHECK(target.allocations() == 3);
}

TEST_CASE("Renderer validates views and closes its pass when upload memory runs out", "[renderer]") {
    // Room for the eight instances' transforms (896 bytes: four 256-byte slices), their order (one), the
    // view's constants (nine), and the material's (one): then the tone map's constants run out, inside its pass.
    CapturingDevice device({3, 4 * 256 + 256 + 9 * 256 + 256});
    TestProject project(device, {{"cube.mesh", unit_cube()}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    World world;
    build_world(world, [&](WorldCommands& commands) {
        for (int i = 0; i < 8; ++i) {
            auto entity = commands.create();
            commands.add(entity, TransformComponent{{float(i) * 0.25f - 1.0f, 0, 0}, {}, {0.2f}}); // all in view
            commands.add(entity, MeshRendererComponent{cube, {}, true});
        }
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    CHECK(renderer.render(snapshot, view_of(8, 8), target).code == RhiError::stale_handle); // never sized
    REQUIRE_FALSE(target.resize(8, 8));
    CHECK(renderer.render(snapshot, view_of(8, 8), target).code == RhiError::wrong_state); // outside a frame
    REQUIRE_FALSE(device.begin_frame());
    CHECK(renderer.render(snapshot, view_of(16, 8), target).code == RhiError::invalid_usage);
    CHECK(device.passes.empty());
    auto corrupt = RenderSnapshot{};
    corrupt.instances.push_back({});
    CHECK(renderer.render(corrupt, view_of(8, 8), target).code == RhiError::invalid_usage); // no mesh or material
    corrupt.materials.push_back({});
    CHECK(renderer.render(corrupt, view_of(8, 8), target).code == RhiError::invalid_usage); // still no mesh

    const auto error = renderer.render(snapshot, view_of(8, 8), target);
    CHECK(error.code == RhiError::out_of_memory);
    CHECK(device.draws.size() == 8);
    REQUIRE(device.passes.size() == 2);
    CHECK(device.passes.back().label == "tone map");
    CHECK_FALSE(device.end_frame()); // the pass was closed, so the frame ends cleanly
    CHECK(device.stats().transient_failures == 1);
}

TEST_CASE("Presentation maps a pixel area to the destination and validates it", "[renderer]") {
    CapturingDevice device;
    auto renderer = Renderer(device, "test source");
    auto view = RenderTarget(device);
    REQUIRE_FALSE(view.resize(50, 40));
    const auto window = device.create_texture({200, 100, Format::bgra8_unorm, TextureUsage::render_target, "window"});
    REQUIRE(window);
    REQUIRE_FALSE(device.begin_frame());
    CHECK(renderer.present(view, window.handle, {150, 0, 60, 100}).code == RhiError::out_of_range);
    CHECK(renderer.present(view, window.handle, {0, 0, 0, 100}).code == RhiError::out_of_range);
    CHECK(renderer.present(view, TextureHandle{}, {0, 0, 1, 1}).code == RhiError::stale_handle);
    REQUIRE_FALSE(renderer.present(view, window.handle, {50, 25, 100, 50}, {0.2, 0.3, 0.4, 1.0}));
    REQUIRE_FALSE(renderer.present(view, window.handle, {0, 0, 200, 100}));
    REQUIRE_FALSE(device.end_frame());
    REQUIRE(device.presents.size() == 2);
    const auto area = device.presents[0].constants.area;
    CHECK(area.x == Approx(-0.5f)); // left
    CHECK(area.y == Approx(-0.5f)); // bottom
    CHECK(area.z == Approx(0.5f)); // right
    CHECK(area.w == Approx(0.5f)); // top
    CHECK(device.presents[0].texture == view.color().slot);
    CHECK(device.presents[1].constants.area.x == Approx(-1.0f));
    CHECK(device.presents[1].constants.area.w == Approx(1.0f));
    REQUIRE(device.passes.size() == 2);
    CHECK(device.passes[0].colors[0].clear_color[2] == 0.4);
    CHECK(renderer.stats().presents == 2);
}

TEST_CASE("Renderer recreates its pipelines in a new device session", "[renderer]") {
    CapturingDevice device;
    auto renderer = Renderer(device, "test source");
    auto view = RenderTarget(device);
    const auto frame = [&] {
        REQUIRE_FALSE(view.resize(8, 8));
        REQUIRE_FALSE(device.begin_frame());
        REQUIRE_FALSE(renderer.render(RenderSnapshot{}, view_of(8, 8), view));
        const auto window = device.create_texture({8, 8, Format::bgra8_unorm, TextureUsage::render_target, "window"});
        REQUIRE_FALSE(renderer.present(view, window.handle, {0, 0, 8, 8}));
        REQUIRE_FALSE(device.end_frame());
        device.destroy(window.handle);
    };
    frame();
    const auto pipelines = device.stats().pipelines;
    CHECK(pipelines == 2); // tone map and present; lit pipelines wait for a surface that needs them
    frame();
    CHECK(device.stats().pipelines == pipelines); // cached per format
    device.shutdown();
    REQUIRE(device.initialize(nullptr));
    frame();
    CHECK(device.stats().pipelines == pipelines);
    CHECK(device.stats().samplers == 4); // presenting's, the texture placeholder's, the split-sum table's, and shadows'
}

TEST_CASE("Debug lines and outlines cost nothing when there are none, and draw each kind twice when there are", "[renderer][debug]") {
    CapturingDevice device;
    TestProject project(device, {{"cube.mesh", unit_cube()}}, {});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    World world;
    build_world(world, [&](WorldCommands& commands) {
        auto entity = commands.create();
        commands.add(entity, TransformComponent{{0, 0, 0}, {}, {1.0f}});
        commands.add(entity, MeshRendererComponent{cube, {}, true});
    });
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device, {Format::rgba8_unorm, true, "view"});
    const auto view = view_of(64, 32);
    const auto render = [&](const RenderSnapshot& snapshot) {
        REQUIRE_FALSE(target.resize(64, 32));
        REQUIRE_FALSE(device.begin_frame());
        REQUIRE_FALSE(renderer.render(snapshot, view, target));
        REQUIRE_FALSE(device.end_frame());
    };

    // Off: no debug pipelines, uploads, or draws.
    render(extract_render_snapshot(world, *project.registry));
    CHECK(device.pipelines_created == 2); // lit and tone map only
    CHECK(device.debug.empty());
    CHECK(device.passes.size() == 2); // no debug pass
    CHECK(renderer.stats().debug_draws == 0);
    auto empty = DebugDraw{};
    auto options = RenderExtractOptions{};
    options.debug = &empty;
    render(extract_render_snapshot(world, *project.registry, options));
    CHECK(device.pipelines_created == 2);
    CHECK(renderer.stats().debug_draws == 0);

    // On: extraction copies the lines and outlines, and each kind is uploaded once and drawn in front
    // of the scene, then faintly behind it.
    auto debug = DebugDraw{};
    debug.line({0, 0, 0}, {1, 2, 3}, {1, 0, 0, 1});
    debug.cross({0, 1, 0}, 0.5f, {0, 1, 0, 1});
    debug.box(math::Affine::from_matrix(math::Mat4::translate({2, 0, 0})), {0.5f, 1.0f, 1.5f}, {0, 0, 1, 1});
    debug.box(math::Affine::from_matrix(math::Mat4::identity()), {1, 1, 1}, {0, 0, 1, 1});
    debug.capsule(math::Affine::from_matrix(math::Mat4::identity()), 0.25f, 0.75f, {1, 1, 0, 0.5f});
    options.debug = &debug;
    const auto snapshot = extract_render_snapshot(world, *project.registry, options);
    debug.clear(); // the snapshot keeps its own copy
    REQUIRE(snapshot.debug.lines.size() == 4);
    REQUIRE(snapshot.debug.shapes.size() == 3);
    device.passes.clear();
    render(snapshot);
    CHECK(device.pipelines_created == 4);
    // After tone mapping, a pass over the view's color tests the lines against the scene's kept depth.
    REQUIRE(device.passes.size() == 3);
    CHECK(device.passes[0].depth->store == StoreAction::store);
    CHECK(device.passes[2].label == "debug lines");
    CHECK(device.passes[2].colors[0].texture == target.color());
    CHECK(device.passes[2].colors[0].load == LoadAction::load);
    CHECK(device.passes[2].depth->texture == target.depth());
    CHECK(device.passes[2].depth->load == LoadAction::load);
    REQUIRE(device.debug.size() == 6); // lines, boxes, capsules; twice each
    for (size_t i = 0; i < 6; ++i) {
        const auto& call = device.debug[i];
        CHECK(call.pipeline == (i < 3 ? "debug lines in front" : "debug lines behind"));
        CHECK(call.constants.viewport.x == 64.0f);
        CHECK(call.constants.viewport.y == 32.0f);
        CHECK(call.constants.viewport.z == view.debug_line_width);
        CHECK(call.constants.viewport.w == Approx(i < 3 ? 1.0f : 0.3f));
    }
    const auto& lines = device.debug[0];
    CHECK(lines.constants.kind[0] == 0);
    CHECK(lines.vertices == 4 * 6);
    CHECK(lines.instances == 1);
    CHECK(lines.data[4] == 1.0f); // the first line's end, then its color
    CHECK(lines.data[6] == 3.0f);
    CHECK(lines.data[8] == 1.0f);
    const auto& boxes = device.debug[1];
    CHECK(boxes.constants.kind[0] == uint32_t(DebugShapeKind::box) + 1);
    CHECK(boxes.vertices == debug_template_lines(DebugShapeKind::box) * 6);
    CHECK(boxes.instances == 2);
    CHECK(boxes.data[12] == 2.0f); // the first box's translation column
    CHECK(boxes.data[17] == 1.0f); // its half extents
    CHECK(boxes.data[18] == 1.5f);
    const auto& capsules = device.debug[2];
    CHECK(capsules.constants.kind[0] == uint32_t(DebugShapeKind::capsule) + 1);
    CHECK(capsules.constants.kind[1] == 16); // several pixels across in this small view
    CHECK(capsules.vertices == debug_template_lines(DebugShapeKind::capsule, 16) * 6);
    CHECK(capsules.data[16] == 0.25f); // radius, half height
    CHECK(capsules.data[17] == 0.75f);
    CHECK(capsules.data[23] == 0.5f); // alpha
    CHECK(renderer.stats().debug_draws == 6);
    CHECK(renderer.stats().debug_lines == 4);
    CHECK(renderer.stats().debug_shapes == 3);
    CHECK(device.draws.size() == 3); // the mesh, in each of the three views
}

TEST_CASE("Sphere and capsule outlines take fewer segments the smaller they are on screen", "[renderer][debug]") {
    CapturingDevice device;
    TestProject project(device, {}, {});
    World world;
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device, {Format::rgba8_unorm, true, "view"});
    const auto view = view_of(640, 360); // from (0, 0, 5), looking at the origin
    auto debug = DebugDraw{};
    debug.sphere(math::Affine::from_matrix(math::Mat4::identity()), 1.0f, {1, 1, 1, 1}); // tens of pixels across
    debug.sphere(math::Affine::from_matrix(math::Mat4::translate({0, 0, -80})), 1.0f, {1, 1, 1, 1}); // a few pixels
    debug.sphere(math::Affine::from_matrix(math::Mat4::scale(math::Vec3(0.2f))), 1.0f, {1, 1, 1, 1}); // its matrix's scale counts
    debug.capsule(math::Affine::from_matrix(math::Mat4::identity()), 0.5f, 1.0f, {1, 1, 1, 1});
    auto options = RenderExtractOptions{};
    options.debug = &debug;
    REQUIRE_FALSE(target.resize(640, 360));
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(renderer.render(extract_render_snapshot(world, *project.registry, options), view, target));
    REQUIRE_FALSE(device.end_frame());
    REQUIRE(device.debug.size() == 8); // spheres at 8, 16, and 32 segments, and the capsule; twice each
    auto segments = std::vector<std::pair<uint32_t, uint32_t>>{};
    for (size_t i = 0; i < 4; ++i) {
        const auto& call = device.debug[i];
        segments.emplace_back(call.constants.kind[0], call.constants.kind[1]);
        CHECK(call.instances == 1);
        CHECK(call.vertices == debug_template_lines(DebugShapeKind(call.constants.kind[0] - 1), call.constants.kind[1]) * 6);
    }
    const auto sphere = uint32_t(DebugShapeKind::sphere) + 1, capsule = uint32_t(DebugShapeKind::capsule) + 1;
    CHECK(segments == std::vector<std::pair<uint32_t, uint32_t>>{{sphere, 8}, {sphere, 16}, {sphere, 32}, {capsule, 32}});
    CHECK(device.debug[0].data[14] == -80.0f); // the far sphere has the fewest
    CHECK(debug_segments_for(5.0f) == 8);
    CHECK(debug_segments_for(12.0f) == 16);
    CHECK(debug_segments_for(100.0f) == 32);
}

TEST_CASE("Debug helpers make the lines they promise", "[renderer][debug]") {
    auto debug = DebugDraw{};
    CHECK(debug.empty());
    debug.cross({1, 2, 3}, 0.2f, {1, 1, 1, 1});
    REQUIRE(debug.lines.size() == 3);
    CHECK(debug.lines[0].from.x == Approx(0.9f));
    CHECK(debug.lines[0].to.x == Approx(1.1f));
    debug.arrow({0, 0, 0}, {0, 0, 2}, {1, 1, 1, 1});
    REQUIRE(debug.lines.size() == 8); // the shaft and four head lines
    for (size_t i = 4; i < 8; ++i) {
        CHECK(debug.lines[i].from.z == 2.0f);
        CHECK(debug.lines[i].to.z == Approx(1.6f)); // a fifth back
    }
    debug.arrow({0, 0, 0}, {0, 0, 0}, {1, 1, 1, 1}); // no length: no head
    CHECK(debug.lines.size() == 9);
    debug.sphere(math::Affine::from_matrix(math::Mat4::identity()), 2.0f, {1, 1, 1, 1});
    CHECK(debug.shapes.back().size.x == 2.0f);
    CHECK(debug_template_lines(DebugShapeKind::box) == 12);
    CHECK(debug_template_lines(DebugShapeKind::sphere) == 96);
    CHECK(debug_template_lines(DebugShapeKind::capsule) == 132);
    debug.clear();
    CHECK(debug.empty());
}

TEST_CASE("Materials choose their pipelines and bind their maps, and blended surfaces draw last, back to front", "[renderer][materials]") {
    CapturingDevice device;
    auto opaque = MaterialAsset{{1, 1, 1, 1}, 0.5f, 0.4f};
    opaque.normal_scale = 0.75f;
    opaque.occlusion_strength = 0.5f;
    opaque.emissive = {1.0f, 0.5f, 0.25f};
    opaque.emissive_strength = 4.0f;
    auto masked = MaterialAsset{};
    masked.alpha_mode = AlphaMode::mask;
    masked.alpha_cutoff = 0.3f;
    masked.double_sided = true;
    auto glass = MaterialAsset{{0.5f, 0.7f, 1.0f, 0.4f}, 0.0f, 0.1f};
    glass.alpha_mode = AlphaMode::blend;
    TestProject project(device, {{"cube.mesh", unit_cube()}},
                        {{"opaque.material", opaque}, {"masked.material", masked}, {"glass.material", glass}},
                        {{"base.texture", solid_image({255, 255, 255, 255})}, {"normal.texture", solid_image({128, 128, 128, 128}, TextureRole::normal)}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    const auto base = project.add<TextureAsset>(20, "base.texture");
    const auto normal = project.add<TextureAsset>(21, "normal.texture");
    // The opaque and masked materials share the base color map.
    opaque.base_color_texture = base;
    opaque.normal_texture = normal;
    masked.base_color_texture = base;
    const auto materials = std::array{project.add<MaterialAsset>(10, "opaque.material"), project.add<MaterialAsset>(11, "masked.material"),
                                      project.add<MaterialAsset>(12, "glass.material")};
    REQUIRE_FALSE(project.registry->publish(materials[0], opaque));
    REQUIRE_FALSE(project.registry->publish(materials[1], masked));
    World world;
    build_world(world, [&](WorldCommands& commands) {
        // Listed near glass first, then far glass, the opaque cube, and the masked one.
        for (const auto& [z, material] : {std::pair{2.0f, 2}, std::pair{-2.0f, 2}, std::pair{0.0f, 0}, std::pair{0.5f, 1}}) {
            auto entity = commands.create();
            commands.add(entity, TransformComponent{{0, 0, z}, {}, {1.0f}});
            commands.add(entity, MeshRendererComponent{cube, materials[size_t(material)], true});
        }
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    REQUIRE(snapshot.diagnostics.empty());
    REQUIRE(snapshot.textures.size() == 2); // one lease per texture, however many materials use it
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(8, 8));
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(renderer.render(snapshot, view_of(8, 8), target));
    REQUIRE_FALSE(device.end_frame());
    REQUIRE(device.draws.size() == 4);
    const auto& d = device.draws;
    CHECK(d[0].pipeline == "lit mesh");
    CHECK(d[1].pipeline == "lit double-sided mesh");
    CHECK(d[2].pipeline == "blended mesh");
    CHECK(d[3].pipeline == "blended mesh");
    // The far glass (z -2) before the near one (z 2): back to front from the camera at z 5.
    CHECK(d[2].constants.model.at(2, 3) == -2.0f);
    CHECK(d[3].constants.model.at(2, 3) == 2.0f);
    // Factors and flags: a bit per map in use, and the alpha mode.
    CHECK(d[0].material.factors.z == 0.75f);
    CHECK(d[0].material.factors.w == 0.5f);
    CHECK(d[0].material.emissive.x == 4.0f); // times its strength
    CHECK(d[0].material.emissive.z == 1.0f);
    CHECK(d[0].material.flags[0] == 0b00101u); // base color and normal
    CHECK(d[0].material.flags[1] == uint32_t(AlphaMode::opaque));
    CHECK(d[1].material.flags[0] == 0b00001u);
    CHECK(d[1].material.flags[1] == uint32_t(AlphaMode::mask));
    CHECK(d[1].material.emissive.w == 0.3f); // the cutoff
    CHECK(d[2].material.flags[0] == 0u);
    CHECK(d[2].material.flags[1] == uint32_t(AlphaMode::blend));
    CHECK(d[2].material.base_color.w == 0.4f);
    // Maps are bound in their slots; empty slots hold the placeholder, which is never sampled.
    const auto base_slot = snapshot.textures[0].value().texture().handle().slot;
    const auto normal_slot = snapshot.textures[1].value().texture().handle().slot;
    CHECK(d[0].textures[size_t(MaterialSlot::base_color)] == base_slot);
    CHECK(d[0].textures[size_t(MaterialSlot::normal)] == normal_slot);
    const auto placeholder = d[0].textures[size_t(MaterialSlot::emissive)];
    CHECK(placeholder != base_slot);
    CHECK(d[1].textures[size_t(MaterialSlot::normal)] == placeholder);
    CHECK(d[2].textures[size_t(MaterialSlot::base_color)] == placeholder);
    CHECK(device.stats().pipelines == 4); // three lit kinds and tone mapping; none for double-sided blending
    CHECK(device.material_binds == 3); // opaque, masked, then glass for both blended draws
}

TEST_CASE("Extraction leases each texture once, and draws the placeholder for missing maps and maps of the wrong role", "[renderer][materials]") {
    CapturingDevice device;
    TestProject project(device, {{"cube.mesh", unit_cube()}}, {{"a.material", {}}, {"b.material", {}}},
                        {{"color.texture", solid_image({255, 0, 0, 255})}, {"data.texture", solid_image({0, 255, 0, 255}, TextureRole::data)}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    const auto color = project.add<TextureAsset>(20, "color.texture");
    const auto data = project.add<TextureAsset>(21, "data.texture");
    const auto missing = project.add<TextureAsset>(22, "missing.texture"); // cataloged, no file
    auto a = MaterialAsset{};
    a.base_color_texture = color;
    a.normal_texture = color; // a color map is not a normal map
    a.metallic_roughness_texture = data;
    a.occlusion_texture = data; // packed maps share a texture
    a.emissive_texture = missing;
    auto b = MaterialAsset{};
    b.base_color_texture = color;
    const auto refs = std::array{project.add<MaterialAsset>(10, "a.material"), project.add<MaterialAsset>(11, "b.material")};
    REQUIRE_FALSE(project.registry->publish(refs[0], a));
    REQUIRE_FALSE(project.registry->publish(refs[1], b));
    World world;
    build_world(world, [&](WorldCommands& commands) {
        for (int i = 0; i < 3; ++i) { // two instances of a, one of b
            auto entity = commands.create(EntityId{1, uint64_t(i + 1)});
            commands.add(entity, TransformComponent{});
            commands.add(entity, MeshRendererComponent{cube, refs[i == 2 ? 1 : 0], true});
        }
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    CHECK(snapshot.textures.size() == 2);
    // Each problem is reported once for the material, naming its first entity.
    REQUIRE(snapshot.diagnostics.size() == 2);
    CHECK(snapshot.diagnostics[0].code == RenderIssue::texture_role);
    CHECK(snapshot.diagnostics[0].asset == color.id);
    CHECK(snapshot.diagnostics[0].message.find("normal map") != std::string::npos);
    CHECK(snapshot.diagnostics[0].entity == EntityId{1, 1});
    CHECK(snapshot.diagnostics[1].code == RenderIssue::missing_texture);
    CHECK(snapshot.diagnostics[1].asset == missing.id);
    const auto& material = snapshot.materials[snapshot.instances[0].material];
    CHECK(material.textures[size_t(MaterialSlot::base_color)] == 0);
    CHECK(material.textures[size_t(MaterialSlot::normal)] == placeholder_texture);
    CHECK(material.textures[size_t(MaterialSlot::metallic_roughness)] == 1);
    CHECK(material.textures[size_t(MaterialSlot::occlusion)] == 1);
    CHECK(material.textures[size_t(MaterialSlot::emissive)] == placeholder_texture);
    CHECK(snapshot.materials[snapshot.instances[2].material].textures[size_t(MaterialSlot::normal)] == no_texture);
    // A snapshot that names a texture it does not hold is refused.
    auto broken = snapshot;
    broken.materials[broken.instances[0].material].textures[0] = 7;
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(8, 8));
    REQUIRE_FALSE(device.begin_frame());
    CHECK(renderer.render(broken, view_of(8, 8), target).code == RhiError::invalid_usage);
    CHECK_FALSE(renderer.render(snapshot, view_of(8, 8), target));
    REQUIRE_FALSE(device.end_frame());
}

TEST_CASE("Material constants and maps are uploaded and bound only when they change between draws", "[renderer][materials]") {
    CapturingDevice device;
    TestProject project(device, {{"cube.mesh", unit_cube()}}, {{"a.material", red}, {"b.material", blue}},
                        {{"one.texture", solid_image({255, 255, 255, 255})}, {"two.texture", solid_image({0, 0, 0, 255})}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    const auto one = project.add<TextureAsset>(20, "one.texture"), two = project.add<TextureAsset>(21, "two.texture");
    const auto a = project.add<MaterialAsset>(10, "a.material"), b = project.add<MaterialAsset>(11, "b.material");
    // Same factors, different maps: still told apart.
    auto first = red, second = red;
    first.base_color_texture = one;
    second.base_color_texture = two;
    REQUIRE_FALSE(project.registry->publish(a, first));
    REQUIRE_FALSE(project.registry->publish(b, second));
    World world;
    build_world(world, [&](WorldCommands& commands) {
        for (const auto material : {a, a, a, b, b, a}) {
            auto entity = commands.create();
            commands.add(entity, TransformComponent{});
            commands.add(entity, MeshRendererComponent{cube, material, true});
        }
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    REQUIRE(snapshot.diagnostics.empty());
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(8, 8));
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(renderer.render(snapshot, view_of(8, 8), target));
    REQUIRE_FALSE(device.end_frame());
    REQUIRE(device.draws.size() == 6);
    const auto slot_of = [&](size_t texture) { return snapshot.textures[texture].value().texture().handle().slot; };
    // Grouped by material (#1025): the four instances of a in one instanced draw, then the two of b.
    REQUIRE(device.draw_calls.size() == 2);
    CHECK(device.draw_calls[0].instances == 4);
    CHECK(device.draw_calls[1].instances == 2);
    const auto expected = std::array{0, 0, 0, 0, 1, 1};
    for (size_t i = 0; i < 6; ++i) {
        INFO("instance " << i);
        CHECK(device.draws[i].textures[0] == slot_of(size_t(expected[i])));
        CHECK(device.draws[i].material.flags[0] == 1u);
    }
    CHECK(device.material_binds == 1); // a and b have the same factors: only their maps are rebound
    CHECK(device.draws[4].material.base_color.x == device.draws[0].material.base_color.x);
}

TEST_CASE("Extraction takes the scene's environment, reports a second one or a missing one, and keeps the ambient otherwise", "[renderer][environments]") {
    CapturingDevice device;
    TestProject project(device, {}, {}, {}, {{"sky.environment", uniform_environment(1.0f, 32)}});
    const auto sky = project.add<EnvironmentAsset>(1, "sky.environment");
    const auto gone = project.add<EnvironmentAsset>(2, "gone.environment"); // cataloged, no file
    const auto extract = [&](std::vector<std::pair<EntityId, EnvironmentComponent>> environments) {
        World world;
        build_world(world, [&](WorldCommands& commands) {
            for (const auto& [id, environment] : environments) commands.add(commands.create(id), environment);
        });
        return extract_render_snapshot(world, *project.registry, {math::Vec3{0.25f}});
    };
    // None, or one with no asset: the ambient light, and nothing to say.
    CHECK_FALSE(extract({}).environment);
    const auto unassigned = extract({{EntityId{1, 1}, EnvironmentComponent{}}});
    CHECK_FALSE(unassigned.environment);
    CHECK(unassigned.diagnostics.empty());
    CHECK(unassigned.ambient.x == 0.25f);
    // One: leased with its settings.
    const auto one = extract({{EntityId{1, 1}, EnvironmentComponent{sky, 3.0f, 0.5f, false}}});
    REQUIRE(one.environment);
    CHECK(one.environment->entity == EntityId{1, 1});
    CHECK(one.environment->asset.reference() == sky);
    CHECK(one.environment->intensity == 3.0f);
    CHECK(one.environment->rotation == 0.5f);
    CHECK_FALSE(one.environment->background);
    CHECK(one.diagnostics.empty());
    // Two: the lowest EntityId's, and the other reported.
    const auto two = extract({{EntityId{1, 9}, EnvironmentComponent{gone}}, {EntityId{1, 2}, EnvironmentComponent{sky}}});
    REQUIRE(two.environment);
    CHECK(two.environment->entity == EntityId{1, 2});
    REQUIRE(two.diagnostics.size() == 1);
    CHECK(two.diagnostics[0].code == RenderIssue::environment_limit);
    CHECK(two.diagnostics[0].entity == EntityId{1, 9});
    // Missing: the ambient light, reported with the asset.
    const auto missing = extract({{EntityId{1, 1}, EnvironmentComponent{gone}}});
    CHECK_FALSE(missing.environment);
    REQUIRE(missing.diagnostics.size() == 1);
    CHECK(missing.diagnostics[0].code == RenderIssue::missing_environment);
    CHECK(missing.diagnostics[0].asset == gone.id);
}

TEST_CASE("Streaming extraction never waits: what is loading is left out or drawn without it, then drawn once loaded", "[renderer][loading]") {
    CapturingDevice device;
    auto material = MaterialAsset{}; // no maps at first
    TestProject project(device, {{"cube.mesh", unit_cube()}}, {{"m.material", material}},
                        {{"color.texture", solid_image({255, 0, 0, 255})}}, {{"sky.environment", uniform_environment(1.0f, 32)}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    const auto color = project.add<TextureAsset>(2, "color.texture");
    const auto sky = project.add<EnvironmentAsset>(3, "sky.environment");
    const auto ref = project.add<MaterialAsset>(4, "m.material");
    material.base_color_texture = color; // published later, as an edit would
    World world;
    build_world(world, [&](WorldCommands& commands) {
        auto entity = commands.create(EntityId{1, 1});
        commands.add(entity, TransformComponent{});
        commands.add(entity, MeshRendererComponent{cube, ref, true});
        commands.add(commands.create(EntityId{1, 2}), EnvironmentComponent{sky});
    });
    auto& registry = *project.registry;
    auto options = RenderExtractOptions{math::Vec3{0.25f}};
    options.loading = AssetLoading::stream;
    const auto extract = [&] {
        registry.update();
        registry.begin_frame();
        auto snapshot = extract_render_snapshot(world, registry, options);
        registry.end_frame();
        return snapshot;
    };

    // Nothing loaded: the cube is left out, the ambient light stands in, and nothing is reported missing.
    const auto first = extract();
    CHECK(first.instances.empty());
    CHECK(first.stats.pending == 1);
    CHECK(first.stats.environment_pending);
    CHECK_FALSE(first.environment);
    CHECK(first.ambient.x == 0.25f);
    CHECK(first.diagnostics.empty());
    CHECK(registry.load_stats().in_flight > 0);
    CHECK(registry.load_stats().waited_in_frames == 0);

    // Loaded: drawn as usual, with nothing pending.
    registry.wait_idle();
    const auto loaded = extract();
    REQUIRE(loaded.instances.size() == 1);
    CHECK(loaded.stats.pending == 0);
    CHECK_FALSE(loaded.stats.environment_pending);
    CHECK(loaded.environment);
    CHECK(loaded.materials[loaded.instances[0].material].textures[size_t(MaterialSlot::base_color)] == no_texture);

    // A material whose map is still loading draws with its factors meanwhile, and with the map after.
    REQUIRE_FALSE(registry.publish(ref, material));
    const auto waiting = extract();
    REQUIRE(waiting.instances.size() == 1);
    CHECK(waiting.stats.pending_textures == 1);
    CHECK(waiting.textures.empty());
    CHECK(waiting.materials[waiting.instances[0].material].textures[size_t(MaterialSlot::base_color)] == no_texture);
    CHECK(waiting.diagnostics.empty());
    registry.wait_idle();
    const auto mapped = extract();
    CHECK(mapped.stats.pending_textures == 0);
    REQUIRE(mapped.textures.size() == 1);
    CHECK(mapped.materials[mapped.instances[0].material].textures[size_t(MaterialSlot::base_color)] == 0);
    CHECK(registry.load_stats().waited_in_frames == 0);

    // Waiting (the default) loads everything before it returns, as before.
    auto fresh = TestProject(device, {{"cube.mesh", unit_cube()}}, {{"m.material", {}}});
    const auto fresh_cube = fresh.add<MeshAsset>(1, "cube.mesh");
    const auto fresh_material = fresh.add<MaterialAsset>(4, "m.material");
    World other;
    build_world(other, [&](WorldCommands& commands) {
        auto entity = commands.create(EntityId{1, 1});
        commands.add(entity, TransformComponent{});
        commands.add(entity, MeshRendererComponent{fresh_cube, fresh_material, true});
    });
    CHECK(extract_render_snapshot(other, *fresh.registry).instances.size() == 1);
}

TEST_CASE("The environment reaches the view constants and its slots, and the sky draws after opaque surfaces and before blended ones", "[renderer][environments]") {
    CapturingDevice device;
    auto glass = MaterialAsset{{1, 1, 1, 0.5f}};
    glass.alpha_mode = AlphaMode::blend;
    TestProject project(device, {{"cube.mesh", unit_cube()}}, {{"solid.material", red}, {"glass.material", glass}}, {},
                        {{"sky.environment", environment_image(64, [](const math::Vec3& d) { return math::Vec3{1.0f + d.y}; })}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    const auto solid = project.add<MaterialAsset>(2, "solid.material"), see_through = project.add<MaterialAsset>(3, "glass.material");
    const auto sky = project.add<EnvironmentAsset>(4, "sky.environment");
    World world;
    build_world(world, [&](WorldCommands& commands) {
        for (const auto material : {see_through, solid}) {
            auto entity = commands.create();
            commands.add(entity, TransformComponent{});
            commands.add(entity, MeshRendererComponent{cube, material, true});
        }
        commands.add(commands.create(), EnvironmentComponent{sky, 2.0f, math::PI / 2, true});
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    REQUIRE(snapshot.environment);
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(8, 8));
    const auto render = [&](const RenderSnapshot& drawn) {
        device.draws.clear();
        device.sequence.clear();
        REQUIRE_FALSE(device.begin_frame());
        REQUIRE_FALSE(renderer.render(drawn, view_of(8, 8), target));
        REQUIRE_FALSE(device.end_frame());
        device.finish_frames();
    };
    render(snapshot);
    CHECK(device.sequence == std::vector<std::string>{"lit mesh", "sky", "blended mesh", "tone map"});
    const auto& view = device.draws.front().view;
    CHECK(view.environment.x == 2.0f);
    CHECK(std::abs(view.environment.y) < 1e-6f); // cos of a quarter turn
    CHECK(view.environment.z == 1.0f);
    const auto& asset = snapshot.environment->asset.value();
    CHECK(view.environment.w == float(asset.specular_levels() - 1));
    CHECK(view.environment_flags[0] == 1);
    CHECK(view.environment_flags[1] == 1);
    for (size_t i = 0; i < 9; ++i) CHECK(view.irradiance[i].x == asset.irradiance()[i].x);
    // The inverse view-projection undoes the view-projection.
    const auto round_trip = view.inverse_view_projection * view.view_projection;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) CHECK(std::abs(round_trip.at(r, c) - (r == c ? 1.0f : 0.0f)) < 1e-4f);
    CHECK(device.bound_textures.at(5) == asset.specular().handle().slot);
    CHECK(device.bound_textures.at(6) == asset.background().handle().slot);
    const auto table = device.bound_textures.at(7);
    // Without the sky the environment still lights the scene; without an environment, an empty cube
    // stands in and the flag is off.
    auto no_sky = snapshot;
    no_sky.environment->background = false;
    render(no_sky);
    CHECK(device.sequence == std::vector<std::string>{"lit mesh", "blended mesh", "tone map"});
    CHECK(device.draws.front().view.environment_flags[0] == 1);
    CHECK(device.draws.front().view.environment_flags[1] == 0);
    auto none = snapshot;
    none.environment.reset();
    render(none);
    CHECK(device.draws.front().view.environment_flags[0] == 0);
    CHECK(device.bound_textures.at(5) != asset.specular().handle().slot);
    CHECK(device.bound_textures.at(7) == table); // one table for every frame
    // A sky with nothing else to draw.
    auto empty = snapshot;
    empty.instances.clear();
    render(empty);
    CHECK(device.sequence == std::vector<std::string>{"sky", "tone map"});
}

TEST_CASE("Point and spot lights come from position, rotation, candela, range, and cones; one directional light casts shadows", "[renderer][lights]") {
    CapturingDevice device;
    TestProject project(device, {});
    World world;
    const auto tilt = math::Quat::from_axis_angle({1, 0, 0}, -math::PI / 2.0f); // local +Z to world +Y: shining down
    build_world(world, [&](WorldCommands& commands) {
        const auto light = [&](EntityId id, LightComponent value, math::Vec3 position = {}) {
            auto entity = commands.create(id);
            commands.add(entity, TransformComponent{position, tilt, {1.0f}});
            commands.add(entity, value);
        };
        auto point = LightComponent{LightKind::point, {1.0f, 0.5f, 0.25f}, 40.0f, 12.0f};
        light({0, 7}, point, {1, 2, 3});
        auto spot = LightComponent{LightKind::spot, {1.0f}, 25.0f, 8.0f, 0.4f, 1.2f};
        spot.shadow_bias = 2.5f;
        spot.shadow_normal_bias = 0.5f;
        light({0, 3}, spot, {0, 5, 0});
        auto quiet = spot;
        quiet.cast_shadows = false;
        light({0, 4}, quiet);
        // Three directional lights asking for shadows: the first by EntityId gets them, the others are reported.
        auto sun = LightComponent{};
        sun.shadow_distance = 80.0f;
        light({0, 20}, sun);
        light({0, 21}, LightComponent{});
        light({0, 22}, LightComponent{});
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry, {});
    REQUIRE(snapshot.local_lights.size() == 3);
    const auto& spot = snapshot.local_lights[0]; // in EntityId order
    CHECK(spot.entity == EntityId{0, 3});
    CHECK(spot.kind == LightKind::spot);
    CHECK(spot.position.y == 5.0f);
    CHECK(spot.direction.y == Approx(-1.0f)); // local -Z
    CHECK(spot.intensity.x == 25.0f); // candela
    CHECK(spot.range == 8.0f);
    CHECK(spot.cos_inner == Approx(std::cos(0.2f))); // half the full angles
    CHECK(spot.cos_outer == Approx(std::cos(0.6f)));
    CHECK(spot.shadow.cast);
    CHECK(spot.shadow.bias == 2.5f);
    CHECK(spot.shadow.normal_bias == 0.5f);
    CHECK_FALSE(snapshot.local_lights[1].shadow.cast);
    const auto& point = snapshot.local_lights[2];
    CHECK(point.kind == LightKind::point);
    CHECK(point.position.z == 3.0f);
    CHECK(point.intensity.y == 20.0f); // color x candela
    CHECK(point.range == 12.0f);
    CHECK_FALSE(point.shadow.cast); // point lights cast no shadows yet
    REQUIRE(snapshot.lights.size() == 3);
    CHECK(snapshot.lights[0].shadow.cast);
    CHECK(snapshot.lights[0].shadow_distance == 80.0f);
    CHECK_FALSE(snapshot.lights[1].shadow.cast);
    CHECK_FALSE(snapshot.lights[2].shadow.cast);
    const auto limited = std::ranges::count(snapshot.diagnostics, RenderIssue::shadow_limit, &RenderDiagnostic::code);
    CHECK(limited == 2);
    CHECK(snapshot.diagnostics.size() == 2);

    // Past the extraction limit, the highest EntityIds are left out and reported.
    World many;
    build_world(many, [&](WorldCommands& commands) {
        for (uint64_t i = 0; i < max_extracted_local_lights + 3; ++i) {
            auto entity = commands.create({0, 1000 + i});
            commands.add(entity, TransformComponent{});
            commands.add(entity, LightComponent{LightKind::point});
        }
    });
    const auto crowded = extract_render_snapshot(many, *project.registry, {});
    CHECK(crowded.local_lights.size() == max_extracted_local_lights);
    CHECK(crowded.local_lights.back().entity == EntityId{0, 1000 + max_extracted_local_lights - 1});
    CHECK(std::ranges::count(crowded.diagnostics, RenderIssue::light_limit, &RenderDiagnostic::code) == 3);
}

TEST_CASE("Debug views cost nothing until chosen, and then draw the same surfaces with their own pipelines", "[renderer][debug-views]") {
    CapturingDevice device;
    TestProject project(device, {{"cube.mesh", unit_cube()}}, {{"solid.material", red}}, {},
                        {{"sky.environment", environment_image(64, [](const math::Vec3&) { return math::Vec3{1.0f}; })}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh");
    const auto solid = project.add<MaterialAsset>(2, "solid.material");
    const auto sky = project.add<EnvironmentAsset>(4, "sky.environment");
    World world;
    build_world(world, [&](WorldCommands& commands) {
        auto entity = commands.create();
        commands.add(entity, TransformComponent{});
        commands.add(entity, MeshRendererComponent{cube, solid, true});
        commands.add(commands.create(), EnvironmentComponent{sky, 1.0f, 0.0f, true});
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(8, 8));
    const auto render = [&](DebugView shown) {
        device.draws.clear();
        device.sequence.clear();
        device.passes.clear();
        device.tone_maps.clear();
        auto view = view_of(8, 8);
        view.debug_view = shown;
        REQUIRE_FALSE(device.begin_frame());
        REQUIRE_FALSE(renderer.render(snapshot, view, target));
        REQUIRE_FALSE(device.end_frame());
        device.finish_frames();
    };
    // Off: no debug pipelines, and exactly the lit frame's passes and draws.
    render(DebugView::none);
    CHECK(device.debug_sources == 0);
    const auto lit_sequence = device.sequence;
    const auto lit_passes = device.passes.size();
    CHECK(lit_sequence == std::vector<std::string>{"lit mesh", "sky", "tone map"});
    CHECK(device.tone_maps.back().constants.view == 0);
    const auto created = device.pipelines_created;
    // The exposure views change only the tone-mapping pass's constants.
    for (const auto [shown, mode] : {std::pair{DebugView::luminance, 1u}, std::pair{DebugView::false_color, 2u}}) {
        render(shown);
        CHECK(device.sequence == lit_sequence);
        CHECK(device.tone_maps.back().constants.view == mode);
    }
    CHECK(device.pipelines_created == created);
    // A material view: the same draws through debug pipelines, no sky, inputs shown as they are.
    render(DebugView::base_color);
    CHECK(device.sequence == std::vector<std::string>{"lit mesh (debug view)", "tone map"});
    CHECK(device.passes.size() == lit_passes);
    CHECK(device.tone_maps.back().constants.view == 3);
    CHECK(device.draws.back().view.shadows.view_forward.w == float(DebugView::base_color));
    CHECK(device.debug_sources == 1);
    // Lighting and shadow views keep the sky, and are light.
    render(DebugView::direct_light);
    CHECK(device.sequence == std::vector<std::string>{"lit mesh (debug view)", "sky", "tone map"});
    CHECK(device.tone_maps.back().constants.view == 0);
    render(DebugView::cascades);
    CHECK(device.sequence == std::vector<std::string>{"lit mesh (debug view)", "sky", "tone map"});
    CHECK(device.debug_sources == 1); // built once
    // Off again: back to the lit pipelines, with nothing more created.
    const auto before = device.pipelines_created;
    render(DebugView::none);
    CHECK(device.sequence == lit_sequence);
    CHECK(device.pipelines_created == before);
}

TEST_CASE("Views and shadow maps draw one instanced draw per mesh and material in them, not one per instance", "[renderer][batches]") {
    CapturingDevice device({3, size_t{4} << 20});
    TestProject project(device, {{"cube.mesh", unit_cube()}, {"slab.mesh", unit_cube()}}, {{"red.material", red}, {"blue.material", blue}});
    const auto cube = project.add<MeshAsset>(1, "cube.mesh"), slab = project.add<MeshAsset>(2, "slab.mesh");
    const auto r = project.add<MaterialAsset>(3, "red.material"), b = project.add<MaterialAsset>(4, "blue.material");
    World world;
    build_world(world, [&](WorldCommands& commands) {
        // 400 small instances in view: every mix of two meshes and two materials, interleaved; and 100 far away.
        for (int i = 0; i < 500; ++i) {
            auto entity = commands.create();
            const auto inside = i < 400;
            const auto x = inside ? float(i % 20) * 0.1f - 1.0f : 1000.0f + float(i);
            commands.add(entity, TransformComponent{{x, float(i / 20 % 20) * 0.1f - 1.0f, 0}, {}, {0.05f}});
            commands.add(entity, MeshRendererComponent{i % 2 ? cube : slab, i % 3 ? r : b, true});
        }
        auto sun = commands.create();
        commands.add(sun, TransformComponent{{}, math::Quat::from_axis_angle({1, 0, 0}, -1.0f), {1.0f}});
        commands.add(sun, LightComponent{});
    });
    const auto snapshot = extract_render_snapshot(world, *project.registry);
    REQUIRE(snapshot.instances.size() == 500);
    CHECK(snapshot.materials.size() == 2); // shared, not copied per instance
    auto renderer = Renderer(device, "test source");
    auto target = RenderTarget(device);
    REQUIRE_FALSE(target.resize(64, 64));
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(renderer.render(snapshot, view_of(64, 64), target));
    REQUIRE_FALSE(device.end_frame());
    // The view: four draws for four mesh-material pairs, 400 instances, the far ones culled.
    CHECK(device.draw_calls.size() == 4);
    CHECK(device.draws.size() == 400);
    CHECK(renderer.last_view().drawn == 400);
    CHECK(renderer.last_view().culled == 100);
    CHECK(renderer.last_view().batches == 4);
    CHECK(renderer.stats().draws == 4);
    CHECK(renderer.stats().instances == 400);
    // Each drawn instance gets its own transform through the order.
    auto seen = std::set<float>{};
    for (const auto& draw : device.draws) seen.insert(draw.constants.model.at(0, 3) * 1000.0f + draw.constants.model.at(1, 3));
    CHECK(seen.size() == 400);
    // Shadow maps: opaque casters by mesh alone, so at most two draws in each of the four cascades.
    CHECK(device.shadow_calls.size() <= 4 * 2);
    CHECK(device.shadow_draws.size() == renderer.stats().shadow_instances);
}
