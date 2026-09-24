#include "maya/assets/property_context.hpp"
#include "maya/scene/scene_io.hpp"
#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

using namespace maya;
namespace fs = std::filesystem;

namespace {
const auto tree_mesh = AssetRef<MeshAsset>{{0x6d617961, 1}};
const auto bark = AssetRef<MaterialAsset>{{0x6d617961, 2}};

class MaterialProvider final : public AssetProvider {
public:
    explicit MaterialProvider(size_t& loads) : m_loads(loads) {}
    AssetLoadResult<MeshAsset> load_mesh(const fs::path&) override {
        return {nullptr, {AssetError::load_failed, "scene tests do not upload meshes"}};
    }
    AssetLoadResult<MaterialAsset> load_material(const fs::path&) override {
        ++m_loads;
        return {std::make_shared<const MaterialAsset>(), {}};
    }
private:
    size_t& m_loads;
};

std::string read_file(const fs::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
void write_file(const fs::path& path, std::string_view text) {
    auto output = std::ofstream(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

struct Project {
    fs::path root = fs::temp_directory_path() /
        ("maya-scene-" + std::to_string(::getpid()) + "-" + std::to_string(detail::next_lifetime_token()));
    size_t material_loads = 0;
    std::unique_ptr<AssetRegistry> registry;
    Project() {
        fs::create_directories(root / "materials");
        write_file(root / "materials/bark.mat", "maya-material 1\n");
        registry = std::make_unique<AssetRegistry>(root, std::make_unique<MaterialProvider>(material_loads));
        REQUIRE_FALSE(registry->register_asset(tree_mesh, "models/tree.obj"));
        REQUIRE_FALSE(registry->register_asset(bark, "materials/bark.mat"));
    }
    ~Project() {
        std::error_code error;
        fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, error);
        fs::remove_all(root, error);
    }
    PropertyValidationContext context() const { return asset_property_context(*registry); }
    std::vector<fs::path> files() const {
        auto names = std::vector<fs::path>{};
        for (const auto& entry : fs::directory_iterator(root)) names.push_back(entry.path().filename());
        std::ranges::sort(names);
        return names;
    }
};

struct SampleIds {
    EntityId camera{0xca, 1}, sun{0x5a, 1}, forest{0xf0, 1}, trees[3]{{0x7e, 3}, {0x7e, 1}, {0x7e, 2}},
        branch{0xb4, 1}, marker{0x3a, 1};
};
constexpr auto tricky_name = "Main \"camera\"\\\n\ttab \x01 del\x7f \xc3\xbc \xf0\x9f\x8c\xb2";

/// Camera, spot light, a forest root with three ordered children, a grandchild, and an empty entity.
void populate(World& world, const SampleIds& ids) {
    auto commands = world.commands();
    const auto camera = commands.create(ids.camera);
    commands.add(camera, NameComponent{tricky_name});
    commands.add(camera, TransformComponent{{0.1f, 1.7f, 5.0f},
        math::Quat::from_axis_angle(math::Vec3(0.0f, 1.0f, 0.0f), 0.3f), {1.0f}});
    commands.add(camera, CameraComponent{1.1f, 0.05f, 750.0f});
    const auto sun = commands.create(ids.sun);
    commands.add(sun, LightComponent{LightKind::spot, {1.0f, 0.9f, 0.7f}, 1600.0f, 25.0f, 0.4f, 0.9f, false});
    const auto forest = commands.create(ids.forest);
    commands.add(forest, NameComponent{"Forest"});
    commands.add(forest, TransformComponent{{-3.0f, 0.0f, 2.5f},
        math::Quat::from_axis_angle(math::Vec3(0.0f, 0.0f, 1.0f), 0.7f), {2.0f, 0.5f, 1.25f}});
    auto trees = std::vector<PendingEntity>{};
    for (size_t i = 0; i < 3; ++i) {
        trees.push_back(commands.create(ids.trees[i]));
        commands.add(trees[i], NameComponent{"Tree " + std::to_string(i)});
        commands.add(trees[i], TransformComponent{{float(i) * 1.5f, 0.0f, -0.1f * float(i)},
            math::Quat::from_axis_angle(math::Vec3(0.3f, 0.8f, 0.52f), 0.123f * float(i + 1)), {1.0f, 1.0f + float(i), 1.0f}});
        commands.add(trees[i], MeshRendererComponent{tree_mesh, bark, i != 1});
    }
    // Reparenting inserts at the head: resulting sibling order is trees[0], trees[1], trees[2].
    for (auto i = trees.size(); i-- > 0;) commands.reparent(trees[i], forest, ReparentPolicy::keep_local);
    const auto branch = commands.create(ids.branch);
    commands.add(branch, TransformComponent{{0.0f, 2.0f, 0.0f}, {}, {0.25f}});
    commands.add(branch, MeshRendererComponent{tree_mesh, {}, true});
    commands.reparent(branch, trees[1], ReparentPolicy::keep_local);
    commands.create(ids.marker);
    REQUIRE(world.commit(commands));
}

bool same(const PropertyValue& left, const PropertyValue& right) {
    if (left.index() != right.index()) return false;
    return std::visit([&]<class T>(const T& value) {
        const auto& other = std::get<T>(right);
        if constexpr (std::same_as<T, math::Vec3>)
            return value.x == other.x && value.y == other.y && value.z == other.z;
        else if constexpr (std::same_as<T, math::Quat>)
            return value.x == other.x && value.y == other.y && value.z == other.z && value.w == other.w;
        else return value == other;
    }, left);
}
std::vector<EntityId> child_ids(const World& world, EntityHandle entity) {
    auto ids = std::vector<EntityId>{};
    for (const auto child : world.children(entity)) ids.push_back(*world.persistent_id(child));
    return ids;
}
/// Exact comparison of persistent state: IDs, schema values, parents, and sibling order.
void require_equivalent(const World& expected, const World& actual) {
    REQUIRE(actual.size() == expected.size());
    expected.for_each_entity([&](EntityHandle entity) {
        const auto id = *expected.persistent_id(entity);
        const auto loaded = actual.find(id);
        REQUIRE(loaded);
        const auto parent = expected.parent(entity);
        const auto loaded_parent = actual.parent(*loaded);
        REQUIRE(parent.has_value() == loaded_parent.has_value());
        if (parent) REQUIRE(*expected.persistent_id(*parent) == *actual.persistent_id(*loaded_parent));
        REQUIRE(child_ids(expected, entity) == child_ids(actual, *loaded));
        for (const auto& schema : component_schemas()) {
            const auto before = read_component(expected, entity, schema.id);
            const auto after = read_component(actual, *loaded, schema.id);
            REQUIRE(before.has_value() == after.has_value());
            if (!before) continue;
            for (const auto& property : schema.properties)
                REQUIRE(same(*read_property(*before, property.id), *read_property(*after, property.id)));
        }
        const auto matrix = expected.world_matrix(entity);
        const auto loaded_matrix = actual.world_matrix(*loaded);
        REQUIRE(matrix.has_value() == loaded_matrix.has_value());
        if (matrix) for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
            REQUIRE(matrix->at(r, c) == loaded_matrix->at(r, c));
    });
}
std::string encode(SceneDocument document, const PropertyValidationContext& context) {
    auto output = std::ostringstream{};
    REQUIRE(write_scene(output, std::move(document), context).empty());
    return output.str();
}

constexpr auto camera_block =
    "maya-scene 1\n"
    "entity 1 1\n"
    "  component maya.camera 1\n"
    "    vertical_fov 1\n"
    "    near_clip 0.1\n"
    "    far_clip 100\n"
    "end\n";
std::string replace(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}
struct Expected {
    std::string text;
    SceneError code;
    size_t line;
    std::string_view excerpt;
};
} // namespace

TEST_CASE("Scenes round-trip IDs, hierarchy order, component values, and shared asset references", "[scene]") {
    Project project;
    const auto ids = SampleIds{};
    World authored;
    populate(authored, ids);
    const auto path = project.root / "forest.scene";
    REQUIRE(save_scene_file(path, capture_scene(authored), project.context()).empty());

    auto reopened = open_scene_file(path, project.context());
    INFO((reopened.diagnostics.empty() ? "" : reopened.diagnostics.front().message));
    REQUIRE(reopened);
    REQUIRE(reopened.world->token() != authored.token());
    require_equivalent(authored, *reopened.world);
    REQUIRE(child_ids(*reopened.world, *reopened.world->find(ids.forest)) ==
            std::vector<EntityId>{ids.trees[0], ids.trees[1], ids.trees[2]});
    reopened.world->with<NameComponent>(*reopened.world->find(ids.camera),
        [](const NameComponent& name) { REQUIRE(name.value == tricky_name); });

    // Byte-identical resave: the format is canonical and revalidation does not drift values.
    const auto first = read_file(path);
    REQUIRE(save_scene_file(path, capture_scene(*reopened.world), project.context()).empty());
    REQUIRE(read_file(path) == first);

    // Shared references resolve to the same registry entry and a single loaded version.
    auto leases = std::vector<AssetLease<MaterialAsset>>{};
    reopened.world->for_each<MeshRendererComponent>([&](EntityHandle, const MeshRendererComponent& renderer) {
        if (!renderer.material.valid()) return;
        REQUIRE(renderer.mesh == tree_mesh);
        auto acquired = project.registry->acquire(renderer.material);
        REQUIRE(acquired);
        leases.push_back(acquired.lease);
    });
    REQUIRE(leases.size() == 3);
    for (const auto& lease : leases) REQUIRE(&lease.value() == &leases.front().value());
    REQUIRE(project.material_loads == 1);
}

TEST_CASE("Scene text contains persistent data only and is independent of runtime layout", "[scene]") {
    Project project;
    const auto ids = SampleIds{};
    World first;
    populate(first, ids);

    // A second World with churned slots/generations and a different creation order.
    World second;
    for (int round = 0; round < 3; ++round) {
        auto churn = second.commands();
        for (int i = 0; i < 7; ++i) churn.create();
        const auto created = second.commit(churn);
        REQUIRE(created);
        auto remove = second.commands();
        for (const auto entity : created.created) remove.destroy(entity);
        REQUIRE(second.commit(remove));
    }
    auto marker = second.commands();
    marker.create(ids.marker);
    REQUIRE(second.commit(marker));
    {
        // Rebuild the remaining content in reverse creation order to vary slot assignment.
        auto document = capture_scene(first);
        std::erase_if(document.entities, [&](const auto& entity) { return entity.id == ids.marker; });
        auto commands = second.commands();
        auto pending = std::vector<std::pair<EntityId, PendingEntity>>{};
        for (auto entity = document.entities.rbegin(); entity != document.entities.rend(); ++entity) {
            const auto created = commands.create(entity->id);
            pending.emplace_back(entity->id, created);
            for (auto value : entity->components)
                std::visit([&](auto& component) { commands.add(created, std::move(component)); }, value);
        }
        const auto find = [&](EntityId id) {
            return std::ranges::find(pending, id, &std::pair<EntityId, PendingEntity>::first)->second;
        };
        for (auto entity = document.entities.rbegin(); entity != document.entities.rend(); ++entity)
            if (entity->parent) commands.reparent(find(entity->id), find(*entity->parent), ReparentPolicy::keep_local);
        REQUIRE(second.commit(commands));
    }
    require_equivalent(first, second);
    const auto text = encode(capture_scene(first), project.context());
    REQUIRE(text == encode(capture_scene(second), project.context()));

    // Every statement is a format keyword or a schema property; no handles or tokens are written.
    auto input = std::istringstream(text);
    auto allowed = std::vector<std::string>{"maya-scene", "entity", "parent", "component", "end"};
    for (const auto& schema : component_schemas())
        for (const auto& property : schema.properties) allowed.emplace_back(property.name);
    for (std::string line; std::getline(input, line);) {
        auto words = std::istringstream(line);
        std::string keyword;
        if (!(words >> keyword)) continue;
        REQUIRE(std::ranges::find(allowed, keyword) != allowed.end());
    }
    REQUIRE(text.find("tree.obj") == std::string::npos); // references are IDs, not locators
}

TEST_CASE("Hand-written scenes accept comments, CRLF, tabs, and normalize rotations", "[scene]") {
    Project project;
    const auto text =
        "# Authored by hand\r\n"
        "maya-scene 1\r\n"
        "\r\n"
        "entity 2a 1\r\n"
        "\tcomponent maya.transform 1\r\n"
        "\t\ttranslation 1 2 3\r\n"
        "\t\trotation 0 0 0 2\r\n"
        "\t\tscale 1 1 1\r\n"
        "\tcomponent maya.mesh_renderer 1\r\n"
        "\t\tmesh 6D617961 1\r\n"
        "\t\tmaterial none\r\n"
        "\t\tvisible true\r\n"
        "end";
    auto loaded = read_scene(std::string_view(text), project.context());
    INFO((loaded.diagnostics.empty() ? "" : loaded.diagnostics.front().message));
    REQUIRE(loaded);
    REQUIRE(loaded.document.entities.size() == 1);
    const auto& transform = std::get<TransformComponent>(loaded.document.entities[0].components[0]);
    REQUIRE(transform.rotation.w == 1.0f);
    REQUIRE(std::get<MeshRendererComponent>(loaded.document.entities[0].components[1]).mesh == tree_mesh);
    REQUIRE_FALSE(std::get<MeshRendererComponent>(loaded.document.entities[0].components[1]).material.valid());
}

TEST_CASE("Malformed and unsupported scene text fails with located diagnostics", "[scene]") {
    Project project;
    const auto block = std::string(camera_block);
    const auto cases = std::vector<Expected>{
        {"", SceneError::malformed, 0, "empty file"},
        {"not a scene\n", SceneError::malformed, 1, "not a Maya scene"},
        {"maya-scene x\n", SceneError::malformed, 1, "header"},
        {"maya-scene 0\n", SceneError::malformed, 1, "header"},
        {"maya-scene 2\n", SceneError::unsupported_version, 1, "newer Maya build"},
        {replace(block, "maya.camera 1", "maya.camera 2"), SceneError::unsupported_version, 3, "maya.camera version 2"},
        {replace(block, "maya.camera 1", "maya.physics 1"), SceneError::unknown_component, 3, "maya.transform"},
        {replace(block, "near_clip", "near"), SceneError::unknown_property, 5, "'near'"},
        {replace(block, "    far_clip 100\n", ""), SceneError::missing_property, 3, "'far_clip'"},
        {replace(block, "far_clip 100", "near_clip 0.2"), SceneError::duplicate_property, 6, "more than once"},
        {replace(block, "end\n", "  component maya.camera 1\nend\n"), SceneError::duplicate_component, 7, "more than one"},
        {replace(block, "0.1", "nan"), SceneError::malformed, 5, "one finite number"},
        {replace(block, "0.1", "1e99"), SceneError::malformed, 5, "one finite number"},
        {replace(block, "0.1", "0.1 0.2"), SceneError::malformed, 5, "one finite number"},
        {replace(block, "end\n", ""), SceneError::malformed, 2, "missing its 'end'"},
        {replace(block, "end\n", "end now\n"), SceneError::malformed, 7, "no arguments"},
        {replace(block, "entity 1 1", "entity 0 0"), SceneError::malformed, 2, "not both zero"},
        {replace(block, "entity 1 1", "entity 1"), SceneError::malformed, 2, "two hexadecimal"},
        {replace(block, "entity 1 1", "entity 1 10000000000000000"), SceneError::malformed, 2, "two hexadecimal"},
        {replace(block, "end\n", "  parent 1 2\nend\n"), SceneError::malformed, 7, "before the entity's components"},
        {replace(block, "entity 1 1\n", ""), SceneError::malformed, 2, "expected 'entity'"},
        {replace(block, "    vertical_fov 1\n", "    vertical_fov 1\n\xff\n"), SceneError::malformed, 5, "UTF-8"},
        {block + "entity 2 2\n  component maya.name 1\n    value Tree\nend\n", SceneError::malformed, 10, "one quoted string"},
        {block + "entity 2 2\n  component maya.name 1\n    value \"Tree\nend\n", SceneError::malformed, 10, "unterminated"},
        {block + "entity 2 2\n  component maya.name 1\n    value \"\\q\"\nend\n", SceneError::malformed, 10, "unsupported escape"},
        {block + "entity 2 2\n  component maya.name 1\n    value \"\\x41\"\nend\n", SceneError::malformed, 10, "control characters"},
        {block + "entity 2 2\n  component maya.mesh_renderer 1\n    mesh 0 0\n    material none\n    visible true\nend\n",
         SceneError::malformed, 10, "'none'"},
        {block + "entity 2 2\n  component maya.light 1\n    kind laser\n    color 1 1 1\n    intensity 1\n    range 1\n"
                 "    inner_cone 0.1\n    outer_cone 0.2\n    enabled true\nend\n", SceneError::malformed, 10, "directional point spot"},
    };
    for (const auto& expected : cases) {
        INFO(expected.text);
        const auto result = read_scene(std::string_view(expected.text), project.context());
        REQUIRE_FALSE(result);
        REQUIRE(result.document.entities.empty());
        REQUIRE(result.diagnostics.size() == 1);
        const auto& diagnostic = result.diagnostics.front();
        INFO(diagnostic.message);
        REQUIRE(diagnostic.code == expected.code);
        REQUIRE(diagnostic.line == expected.line);
        REQUIRE(diagnostic.message.find(expected.excerpt) != std::string::npos);
    }
}

TEST_CASE("Scene validation reports every duplicate, hierarchy, value, and asset problem", "[scene]") {
    Project project;
    const auto text = std::string(
        "maya-scene 1\n"
        "entity a 1\n"                                        // 2
        "  component maya.name 1\n"
        "    value \"First\"\n"
        "end\n"
        "entity a 1\n"                                        // 6: duplicate identity
        "end\n"
        "entity b 1\n"                                        // 8
        "  component maya.mesh_renderer 1\n"
        "    mesh 6d617961 63\n"                              // 10: not in the catalog
        "    material none\n"
        "    visible true\n"
        "end\n"
        "entity b 2\n"                                        // 14
        "  component maya.mesh_renderer 1\n"
        "    mesh none\n"
        "    material 6d617961 1\n"                           // 17: a mesh, not a material
        "    visible true\n"
        "end\n"
        "entity c 1\n"                                        // 20
        "  component maya.camera 1\n"
        "    vertical_fov 1\n"
        "    near_clip -1\n"                                  // 23: out of range
        "    far_clip 100\n"
        "end\n"
        "entity d 1\n"                                        // 26
        "  parent e 1\n"                                      // 27: parent is missing
        "  component maya.transform 1\n"
        "    translation 0 0 0\n    rotation 0 0 0 1\n    scale 1 1 1\n"
        "end\n"
        "entity f 1\n"                                        // 33
        "  parent a 1\n"                                      // 34: neither entity has a transform
        "end\n");
    const auto result = read_scene(std::string_view(text), project.context());
    REQUIRE_FALSE(result);
    REQUIRE(result.document.entities.empty());
    const auto find = [&](SceneError code) {
        const auto found = std::ranges::find(result.diagnostics, code, &SceneDiagnostic::code);
        REQUIRE(found != result.diagnostics.end());
        INFO(found->message);
        return *found;
    };
    const auto duplicate = find(SceneError::duplicate_entity);
    REQUIRE(duplicate.line == 6);
    REQUIRE(duplicate.entity == EntityId{0xa, 1});
    REQUIRE(duplicate.message.find("first on line 2") != std::string::npos);
    const auto missing = find(SceneError::missing_asset);
    REQUIRE(missing.line == 10);
    REQUIRE(missing.message.find("6d617961:63") != std::string::npos);
    REQUIRE(missing.message.find("restore its catalog entry") != std::string::npos);
    const auto wrong = find(SceneError::wrong_asset_type);
    REQUIRE(wrong.line == 17);
    REQUIRE(wrong.message.find("material asset") != std::string::npos);
    REQUIRE(find(SceneError::invalid_value).line == 23);
    const auto hierarchy = std::ranges::count(result.diagnostics, SceneError::invalid_hierarchy, &SceneDiagnostic::code);
    REQUIRE(hierarchy == 3);
    REQUIRE(std::ranges::any_of(result.diagnostics, [](const auto& diagnostic) {
        return diagnostic.line == 27 && diagnostic.message.find("not in this scene") != std::string::npos;
    }));
    REQUIRE(std::ranges::any_of(result.diagnostics, [](const auto& diagnostic) {
        return diagnostic.line == 34 && diagnostic.message.find("'First'") != std::string::npos;
    }));

    // Without a catalog, nonempty references cannot be validated.
    const auto uncataloged = read_scene(std::string_view(
        "maya-scene 1\nentity 1 1\n  component maya.mesh_renderer 1\n    mesh 6d617961 1\n"
        "    material none\n    visible true\nend\n"), {});
    REQUIRE(uncataloged.diagnostics.size() == 1);
    REQUIRE(uncataloged.diagnostics[0].code == SceneError::validation_context_required);

    // In-memory documents get the same checks, including hierarchy cycles and invalid text.
    auto document = SceneDocument{};
    document.entities.push_back({{1, 1}, EntityId{1, 2}, {TransformComponent{}}});
    document.entities.push_back({{1, 2}, EntityId{1, 1}, {TransformComponent{}, NameComponent{"\xc3"}}});
    document.entities.push_back({{1, 3}, EntityId{1, 3}, {TransformComponent{}}});
    const auto problems = validate_scene(document, project.context());
    REQUIRE(std::ranges::count(problems, SceneError::invalid_hierarchy, &SceneDiagnostic::code) == 2);
    REQUIRE(std::ranges::count(problems, SceneError::invalid_value, &SceneDiagnostic::code) == 1);
    REQUIRE_FALSE(instantiate_scene(document, project.context()));
}

TEST_CASE("Scene diagnostics are bounded", "[scene]") {
    Project project;
    auto text = std::string("maya-scene 1\n");
    for (int i = 1; i <= 200; ++i)
        text += "entity 1 " + std::to_string(i) + "\n  component maya.camera 1\n    vertical_fov 1\n"
                "    near_clip 0\n    far_clip 1\nend\n";
    const auto result = read_scene(std::string_view(text), project.context());
    REQUIRE(result.diagnostics.size() == max_scene_diagnostics);
    REQUIRE(result.diagnostics.back().message.find("omitted") != std::string::npos);
}

TEST_CASE("Failed loads keep the active scene", "[scene]") {
    Project project;
    const auto ids = SampleIds{};
    World authored;
    populate(authored, ids);
    const auto good = project.root / "good.scene";
    REQUIRE(save_scene_file(good, capture_scene(authored), project.context()).empty());
    auto active = std::move(open_scene_file(good, project.context()).world);
    REQUIRE(active);
    const auto* original = active.get();

    const auto corrupt = project.root / "corrupt.scene";
    write_file(corrupt, read_file(good).substr(0, read_file(good).size() / 2));
    fs::create_directory(project.root / "folder.scene");
    const auto failures = std::vector<std::pair<fs::path, SceneError>>{
        {corrupt, SceneError::malformed},
        {project.root / "missing.scene", SceneError::io_error},
        {project.root / "folder.scene", SceneError::io_error},
    };
    for (const auto& [path, code] : failures) {
        auto attempt = open_scene_file(path, project.context());
        if (attempt) active = std::move(attempt.world);
        REQUIRE_FALSE(attempt);
        REQUIRE(attempt.diagnostics.front().code == code);
        REQUIRE(attempt.diagnostics.front().message.starts_with(path.string() + ": "));
    }
    REQUIRE(active.get() == original);
    require_equivalent(authored, *active);

    // A catalog missing an asset rejects the scene before any World is built.
    AssetRegistry partial(project.root, std::make_unique<MaterialProvider>(project.material_loads));
    REQUIRE_FALSE(partial.register_asset(bark, "materials/bark.mat"));
    const auto missing = open_scene_file(good, asset_property_context(partial));
    REQUIRE_FALSE(missing);
    REQUIRE(missing.diagnostics.size() == 4); // three trees and the branch reference the mesh
    for (const auto& diagnostic : missing.diagnostics) REQUIRE(diagnostic.code == SceneError::missing_asset);
    REQUIRE(project.material_loads == 0); // validation never loads assets
}

TEST_CASE("Failed saves keep the last valid file", "[scene]") {
    Project project;
    const auto ids = SampleIds{};
    World authored;
    populate(authored, ids);
    const auto path = project.root / "level.scene";
    REQUIRE(save_scene_file(path, capture_scene(authored), project.context()).empty());
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
    const auto last_valid = read_file(path);
    const auto baseline = project.files();

    SECTION("invalid documents write nothing") {
        auto document = capture_scene(authored);
        document.entities.push_back(document.entities.front());
        const auto diagnostics = save_scene_file(path, document, project.context());
        REQUIRE(diagnostics.front().code == SceneError::duplicate_entity);
        REQUIRE(diagnostics.front().message.starts_with(path.string() + ": "));

        document = capture_scene(authored);
        const auto camera = std::ranges::find(document.entities, ids.camera, &SceneEntity::id);
        std::get<NameComponent>(camera->components.front()).value = "\xff";
        REQUIRE(save_scene_file(path, document, project.context()).front().code == SceneError::invalid_value);

        document = capture_scene(authored);
        document.entities.push_back({{9, 9}, {}, {MeshRendererComponent{{{0x6d617961, 64}}, {}, true}}});
        REQUIRE(save_scene_file(path, document, project.context()).front().code == SceneError::missing_asset);
    }
    SECTION("an unwritable directory") {
        if (::geteuid() == 0) SKIP("root ignores directory permissions");
        fs::permissions(project.root, fs::perms::owner_read | fs::perms::owner_exec);
        const auto diagnostics = save_scene_file(path, capture_scene(authored), project.context());
        fs::permissions(project.root, fs::perms::owner_all);
        REQUIRE(diagnostics.size() == 1);
        REQUIRE(diagnostics[0].code == SceneError::io_error);
        REQUIRE(diagnostics[0].message.find("was not changed") != std::string::npos);
    }
    SECTION("a write that fails part way") {
        // The file-size limit makes write() fail with EFBIG after a partial temporary file.
        auto limit = rlimit{};
        REQUIRE(::getrlimit(RLIMIT_FSIZE, &limit) == 0);
        const auto previous_limit = limit;
        const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);
        limit.rlim_cur = 64;
        REQUIRE(::setrlimit(RLIMIT_FSIZE, &limit) == 0);
        const auto diagnostics = save_scene_file(path, capture_scene(authored), project.context());
        ::setrlimit(RLIMIT_FSIZE, &previous_limit);
        std::signal(SIGXFSZ, previous_handler);
        REQUIRE(diagnostics.size() == 1);
        REQUIRE(diagnostics[0].code == SceneError::io_error);
        REQUIRE(diagnostics[0].message.find("cannot write") != std::string::npos);
    }
    SECTION("destinations that are not writable files") {
        REQUIRE(save_scene_file(project.root / "missing/level.scene", capture_scene(authored),
                                project.context()).front().code == SceneError::io_error);
        REQUIRE_FALSE(fs::exists(project.root / "missing"));
        fs::create_directory(project.root / "folder.scene");
        REQUIRE(save_scene_file(project.root / "folder.scene", capture_scene(authored),
                                project.context()).front().code == SceneError::io_error);
        fs::remove(project.root / "folder.scene");
    }
    REQUIRE(read_file(path) == last_valid);
    REQUIRE(project.files() == baseline); // no temporary files left behind
    REQUIRE(open_scene_file(path, project.context()));

    // A successful save replaces the content and keeps the file's permissions.
    auto edit = authored.commands();
    edit.destroy(*authored.find(ids.marker));
    REQUIRE(authored.commit(edit));
    REQUIRE(save_scene_file(path, capture_scene(authored), project.context()).empty());
    REQUIRE(read_file(path) != last_valid);
    REQUIRE((fs::status(path).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write));
    REQUIRE(project.files() == baseline);
}

TEST_CASE("Deep hierarchies and large scenes round-trip without recursion", "[scene]") {
    Project project;
    World authored;
    auto commands = authored.commands();
    auto previous = std::optional<PendingEntity>{};
    for (uint64_t i = 1; i <= 4096; ++i) {
        const auto entity = commands.create({0xdee9, i});
        commands.add(entity, TransformComponent{{0.0f, 0.001f, 0.0f}, {}, {1.0f}});
        if (previous) commands.reparent(entity, *previous, ReparentPolicy::keep_local);
        previous = entity;
    }
    for (uint64_t i = 1; i <= 2000; ++i) {
        const auto entity = commands.create({0xf1a7, i});
        commands.add(entity, NameComponent{"Rock " + std::to_string(i)});
        commands.add(entity, TransformComponent{{float(i), 0.0f, 0.0f}, {}, {1.0f}});
        commands.add(entity, MeshRendererComponent{tree_mesh, bark, true});
    }
    REQUIRE(authored.commit(commands));
    const auto text = encode(capture_scene(authored), project.context());
    auto loaded = read_scene(std::string_view(text), project.context());
    REQUIRE(loaded);
    auto rebuilt = instantiate_scene(std::move(loaded.document), project.context());
    REQUIRE(rebuilt);
    require_equivalent(authored, *rebuilt.world);
}

TEST_CASE("Scene format keywords are reserved from property names", "[scene]") {
    for (const auto& schema : component_schemas())
        for (const auto& property : schema.properties)
            for (const auto keyword : {"maya-scene", "entity", "parent", "component", "end"})
                REQUIRE(property.name != keyword);
}
