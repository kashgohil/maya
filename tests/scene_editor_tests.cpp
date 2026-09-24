#include "scene_editor.hpp"
#include "maya/scene/scene_io.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <random>
#include <sstream>

using namespace maya;
using namespace maya::editor;
using Catch::Approx;

namespace {
/// Canonical scene text: equal text means equal authored state (IDs, hierarchy, sibling order, values).
std::string text(const World& world) {
    auto out = std::ostringstream{};
    const auto problems = write_scene(out, capture_scene(world), {});
    REQUIRE(problems.empty());
    return out.str();
}

std::unique_ptr<World> scene(std::vector<std::pair<std::string, std::optional<int>>> entities,
                             std::vector<EntityId>* ids = nullptr) {
    auto world = std::make_unique<World>();
    auto commands = world->commands();
    auto pending = std::vector<PendingEntity>{};
    auto created = std::vector<EntityId>{};
    for (const auto& [name, parent] : entities) {
        // Ascending IDs, so the initial root order (by ID) matches the order given here.
        created.push_back(EntityId{0x5ced, created.size() + 1});
        pending.push_back(commands.create(created.back()));
        commands.add(pending.back(), NameComponent{name});
        commands.add(pending.back(), TransformComponent{{float(pending.size()), 0, 0}, {}, {1.0f}});
    }
    for (auto i = entities.size(); i-- > 0;)
        if (entities[i].second) commands.reparent(pending[i], pending[size_t(*entities[i].second)], ReparentPolicy::keep_local);
    REQUIRE(world->commit(commands));
    if (ids) *ids = created;
    return world;
}

/// The editor's mirror matches the World and its selection only names live entities.
void check_consistent(const SceneEditor& editor) {
    const auto captured = capture_state(editor.world());
    REQUIRE(captured.entities.size() == editor.world().size());
    for (const auto& [id, record] : captured.entities) {
        const auto* mirrored = editor.record(id);
        REQUIRE(mirrored);
        CHECK(mirrored->parent == record.parent);
        CHECK(editor.children(id) == captured.children_of(id));
    }
    auto roots = editor.roots(), expected = captured.children_of({});
    std::ranges::sort(roots);
    std::ranges::sort(expected);
    CHECK(roots == expected);
    for (const auto id : editor.selection()) CHECK(editor.world().find(id));
}

math::Mat4 world_matrix(const SceneEditor& editor, EntityId id) {
    const auto matrix = editor.world().world_matrix(*editor.world().find(id));
    REQUIRE(matrix);
    return *matrix;
}
} // namespace

TEST_CASE("Create, rename, and subtree deletion undo and redo by persistent ID", "[editor][history]") {
    auto editor = SceneEditor(scene({{"Existing", {}}}));
    const auto initial = text(editor.world());
    REQUIRE(editor.create("Parent"));
    const auto parent = *editor.primary();
    REQUIRE(editor.create("Child", parent));
    const auto child = *editor.primary();
    REQUIRE(editor.rename(child, "Renamed"));
    CHECK(editor.display_name(child) == "Renamed");
    editor.select(parent);
    REQUIRE(editor.delete_selection());
    CHECK_FALSE(editor.world().find(parent));
    CHECK_FALSE(editor.world().find(child)); // the whole subtree
    CHECK(editor.selection().empty());
    const auto final = text(editor.world());
    CHECK(editor.history_size() == 4);
    CHECK(editor.undo_label() == "Delete Parent");

    REQUIRE(editor.undo());
    CHECK(editor.world().find(child)); // the same IDs come back
    CHECK(editor.display_name(child) == "Renamed");
    CHECK(editor.selection() == std::vector{parent}); // the selection before the deletion
    REQUIRE(editor.undo());
    CHECK(editor.display_name(child) == "Child");
    REQUIRE(editor.undo());
    REQUIRE(editor.undo());
    CHECK(text(editor.world()) == initial);
    CHECK_FALSE(editor.can_undo());
    CHECK_FALSE(editor.undo());
    for (int i = 0; i < 4; ++i) REQUIRE(editor.redo());
    CHECK(text(editor.world()) == final);
    CHECK_FALSE(editor.redo());
    check_consistent(editor);

    // Rejected edits change nothing and record nothing.
    CHECK_FALSE(editor.rename(parent, "Gone")); // deleted again by redo
    CHECK_FALSE(editor.create("Orphan", parent));
    const auto existing = editor.roots().front();
    CHECK_FALSE(editor.rename(existing, editor.display_name(existing))); // no change, no step
    CHECK(editor.history_size() == 4);
}

TEST_CASE("Duplication copies subtrees with new IDs right after the originals", "[editor][history]") {
    auto ids = std::vector<EntityId>{};
    // 0 Root, 1 Parent, 2 Child A (of 1), 3 Grandchild (of 2), 4 Child B (of 1)
    auto editor = SceneEditor(scene({{"Root", {}}, {"Parent", {}}, {"Child A", 1}, {"Grandchild", 2}, {"Child B", 1}}, &ids));
    const auto initial = text(editor.world());
    editor.select(ids[1]);
    editor.select(ids[3], SelectMode::add); // inside the selected subtree: copied once, with its parent
    REQUIRE(editor.duplicate_selection());
    REQUIRE(editor.selection().size() == 1);
    const auto copy = editor.selection().front();
    CHECK(copy != ids[1]);
    CHECK(editor.display_name(copy) == "Parent (1)");
    const auto& roots = editor.roots();
    CHECK(std::ranges::find(roots, copy) == std::ranges::find(roots, ids[1]) + 1);
    // Children are copied in order, with new IDs and parents remapped into the copy.
    const auto& children = editor.children(copy);
    REQUIRE(children.size() == 2);
    CHECK(editor.display_name(children[0]) == "Child A");
    CHECK(editor.display_name(children[1]) == "Child B");
    CHECK(children[0] != ids[2]);
    REQUIRE(editor.children(children[0]).size() == 1);
    const auto grandchild = editor.children(children[0]).front();
    CHECK(grandchild != ids[3]);
    CHECK(editor.record(grandchild)->parent == children[0]);
    CHECK(editor.world().size() == 9);
    // The originals are untouched, and poses match their originals.
    CHECK(editor.children(ids[1]) == std::vector{ids[2], ids[4]});
    CHECK(world_matrix(editor, grandchild).at(0, 3) == world_matrix(editor, ids[3]).at(0, 3));
    // A second copy takes the next free number.
    editor.select(ids[1]);
    REQUIRE(editor.duplicate_selection());
    CHECK(editor.display_name(editor.selection().front()) == "Parent (2)");
    REQUIRE(editor.undo());
    REQUIRE(editor.undo());
    CHECK(text(editor.world()) == initial);
    REQUIRE(editor.redo());
    CHECK(editor.world().find(grandchild)); // redo restores the same copy IDs
    CHECK(editor.selection() == std::vector{copy});
    check_consistent(editor);
}

TEST_CASE("Moving keeps world poses, orders siblings, and rejects invalid parents", "[editor][history]") {
    auto ids = std::vector<EntityId>{};
    auto editor = SceneEditor(scene({{"A", {}}, {"B", {}}, {"C", 1}, {"D", 1}, {"E", 1}}, &ids));
    const auto initial = text(editor.world());
    const auto pose = world_matrix(editor, ids[2]);
    // Out to the root, keeping the world pose.
    REQUIRE(editor.move(ids[2], std::nullopt));
    CHECK_FALSE(editor.record(ids[2])->parent);
    CHECK(editor.roots().back() == ids[2]);
    for (int i = 0; i < 16; ++i) CHECK(world_matrix(editor, ids[2]).elements[i] == Approx(pose.elements[i]).margin(1e-5));
    // Into another parent, then before and after siblings.
    REQUIRE(editor.move(ids[2], ids[0]));
    CHECK(editor.children(ids[0]) == std::vector{ids[2]});
    for (int i = 0; i < 16; ++i) CHECK(world_matrix(editor, ids[2]).elements[i] == Approx(pose.elements[i]).margin(1e-5));
    REQUIRE(editor.move(ids[4], ids[3], Placement::before));
    CHECK(editor.children(ids[1]) == std::vector{ids[4], ids[3]});
    REQUIRE(editor.move(ids[4], ids[3], Placement::after));
    CHECK(editor.children(ids[1]) == std::vector{ids[3], ids[4]});
    CHECK(editor.undo_label() == "Reorder E");
    REQUIRE(editor.move(ids[1], ids[0], Placement::before)); // root order is editor state
    CHECK(editor.roots().front() == ids[1]);
    const auto history = editor.history_size();
    const auto before_rejections = text(editor.world());
    CHECK_FALSE(editor.move(ids[1], ids[3])); // under its own child: a cycle
    CHECK_FALSE(editor.move(ids[1], ids[1]));
    CHECK(text(editor.world()) == before_rejections);
    CHECK(editor.history_size() == history);
    // A rotated parent with nonuniform scale would force shear on a rotated child.
    REQUIRE(editor.set_component(ids[0], TransformComponent{{}, math::Quat::from_axis_angle({0, 0, 1}, 0.6f), {1, 3, 1}}));
    REQUIRE(editor.set_component(ids[3], TransformComponent{{}, math::Quat::from_axis_angle({0, 1, 0}, 0.8f), {1.0f}}));
    const auto sheared = text(editor.world());
    const auto rejected = editor.move(ids[3], ids[0]);
    CHECK_FALSE(rejected);
    CHECK(rejected.error.find("shear") != std::string::npos);
    CHECK(text(editor.world()) == sheared);
    CHECK(editor.history_size() == history + 2);
    while (editor.can_undo()) REQUIRE(editor.undo());
    CHECK(text(editor.world()) == initial);
    check_consistent(editor);

    // An entity without a transform cannot join the hierarchy, as child or parent.
    auto world = std::make_unique<World>();
    auto commands = world->commands();
    const auto bare_id = EntityId::generate();
    commands.add(commands.create(bare_id), NameComponent{"Bare"});
    REQUIRE(world->commit(commands));
    auto other = SceneEditor(std::move(world));
    REQUIRE(other.create("Parent"));
    const auto parent = *other.primary();
    CHECK_FALSE(other.move(bare_id, parent));
    CHECK_FALSE(other.move(parent, bare_id));
    CHECK_FALSE(other.create("Child", bare_id));
    REQUIRE(other.move(bare_id, parent, Placement::after)); // reordering among roots is fine
    CHECK(other.roots().back() == bare_id);
}

TEST_CASE("Groups record one step, and dirty state follows the saved history position", "[editor][history]") {
    auto editor = SceneEditor(scene({{"A", {}}}));
    CHECK_FALSE(editor.dirty());
    editor.begin_group("Build a rig");
    REQUIRE(editor.create("Rig"));
    const auto rig = *editor.primary();
    editor.begin_group("nested");
    REQUIRE(editor.create("Arm", rig));
    REQUIRE(editor.rename(rig, "Rig root"));
    editor.end_group();
    CHECK_FALSE(editor.can_undo()); // still grouping
    editor.end_group();
    CHECK(editor.history_size() == 1);
    CHECK(editor.undo_label() == "Build a rig");
    CHECK(editor.dirty());
    REQUIRE(editor.undo());
    CHECK_FALSE(editor.world().find(rig));
    CHECK(editor.world().size() == 1);
    CHECK_FALSE(editor.dirty()); // back at the opened state
    REQUIRE(editor.redo());
    CHECK(editor.dirty());
    editor.mark_saved();
    CHECK_FALSE(editor.dirty());
    REQUIRE(editor.undo());
    CHECK(editor.dirty());
    REQUIRE(editor.redo());
    CHECK_FALSE(editor.dirty());
    // Editing after an undo discards the redo branch; if the saved state was on it, it is unreachable.
    REQUIRE(editor.undo());
    REQUIRE(editor.create("Other"));
    CHECK(editor.dirty());
    REQUIRE(editor.undo());
    CHECK(editor.dirty());
    // An empty group records nothing.
    editor.begin_group("Nothing");
    editor.end_group();
    CHECK(editor.history_size() == 1);
}

TEST_CASE("Selection follows edits, undo, and redo and never names missing entities", "[editor][history]") {
    auto ids = std::vector<EntityId>{};
    auto editor = SceneEditor(scene({{"A", {}}, {"B", {}}, {"C", 1}}, &ids));
    editor.select(ids[0]);
    editor.select(ids[1], SelectMode::toggle);
    editor.select(ids[1], SelectMode::toggle);
    CHECK(editor.selection() == std::vector{ids[0]});
    editor.select(ids[2], SelectMode::add);
    CHECK(editor.primary() == ids[2]);
    editor.select(EntityId::generate()); // unknown IDs are ignored
    CHECK(editor.selection().size() == 2);
    REQUIRE(editor.delete_selection());
    CHECK(editor.selection().empty());
    REQUIRE(editor.undo());
    CHECK(editor.selection() == std::vector{ids[0], ids[2]});
    // Undoing the creation of a selected entity drops it from the selection.
    REQUIRE(editor.create("New"));
    const auto created = *editor.primary();
    editor.select(ids[1], SelectMode::add);
    REQUIRE(editor.undo());
    CHECK_FALSE(editor.selected(created));
    editor.set_selection({created, ids[1], ids[1]});
    CHECK(editor.selection() == std::vector{ids[1]});
}

TEST_CASE("History keeps the most recent steps", "[editor][history]") {
    auto ids = std::vector<EntityId>{};
    auto editor = SceneEditor(scene({{"A", {}}}, &ids));
    for (size_t i = 0; i < SceneEditor::history_limit + 20; ++i) REQUIRE(editor.rename(ids[0], "Name " + std::to_string(i)));
    CHECK(editor.history_size() == SceneEditor::history_limit);
    size_t undone = 0;
    while (editor.undo()) ++undone;
    CHECK(undone == SceneEditor::history_limit);
    CHECK(editor.display_name(ids[0]) == "Name 19");
    CHECK(editor.dirty()); // the opened state fell out of the history
}

TEST_CASE("Random edit sequences undo and redo exactly", "[editor][history]") {
    for (const auto seed : {1u, 7u, 42u, 1234u}) {
        INFO("seed " << seed);
        auto random = std::mt19937(seed);
        const auto pick = [&](size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(random); };
        auto editor = SceneEditor(scene({{"A", {}}, {"B", 0}, {"C", 0}, {"D", {}}}));
        auto next_id = uint64_t{1000};
        editor.set_id_source([&] { return EntityId{0xf022, next_id++}; }); // reproducible from the seed
        const auto initial = text(editor.world());
        const auto any_entity = [&]() -> std::optional<EntityId> {
            auto all = std::vector<EntityId>{};
            for (const auto& [id, record] : capture_state(editor.world()).entities) all.push_back(id);
            if (all.empty()) return std::nullopt;
            std::ranges::sort(all);
            return all[pick(all.size())];
        };
        for (int step = 0; step < 250; ++step) {
            switch (pick(8)) {
            case 0: editor.create("E" + std::to_string(step), pick(2) ? any_entity() : std::nullopt); break;
            case 1: if (const auto id = any_entity()) editor.rename(*id, "R" + std::to_string(step)); break;
            case 2: if (const auto id = any_entity()) { editor.select(*id); editor.duplicate_selection(); } break;
            case 3: if (const auto id = any_entity()) { editor.select(*id); editor.delete_selection(); } break;
            case 4:
                if (const auto id = any_entity())
                    editor.move(*id, pick(3) ? any_entity() : std::nullopt, static_cast<Placement>(pick(3)));
                break;
            case 5: editor.undo(); break;
            case 6: editor.redo(); break;
            case 7: if (const auto id = any_entity()) editor.select(*id, SelectMode::toggle); break;
            }
            check_consistent(editor);
        }
        const auto final = text(editor.world());
        auto undone = 0;
        while (editor.undo()) ++undone;
        CHECK(text(editor.world()) == initial);
        for (int i = 0; i < undone; ++i) REQUIRE(editor.redo());
        CHECK(text(editor.world()) == final);
        check_consistent(editor);
    }
}
