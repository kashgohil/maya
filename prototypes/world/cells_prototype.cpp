// Cell prototype for the world-scale decisions (#1060, docs/architecture/world-scale-decision.md).
//
// What a cell costs to load and to activate, with Maya's real scene format and World:
//   1. format: cells of 500, 2,000, and 8,000 entities (a transform and a mesh renderer each, a collider on
//      a quarter, a name on a tenth) as scene text, against a packed binary cook of the same entities:
//      bytes, parse (or decode), and validation, and reading each from its own file;
//   2. activation: adding a cell's entities to a World that already holds 100,000, in slices of 250, 1,000,
//      and 4,000 entities a frame, and removing them again; and whether a commit's cost depends on the size
//      of the World it joins (10,000, 100,000, 400,000 entities);
//   3. physics: adding and removing a cell's 1,000 colliders in a Jolt world of 50,000 bodies, one at a
//      time against Jolt's batch calls.
// Lines starting UNEXPECTED report what the decision relies on and make the run fail.

#include "world/jolt_scene.hpp"

#include "maya/scene/scene_io.hpp"
#include "maya/world/world.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace maya;
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
int failures = 0;
void unexpected(const std::string& what) {
    std::printf("UNEXPECTED: %s\n", what.c_str());
    ++failures;
}
template<class F> double best_of(int runs, F&& f) {
    auto best = 1e30;
    for (int i = 0; i < runs; ++i) {
        const auto start = Clock::now();
        f();
        best = std::min(best, ms_since(start));
    }
    return best;
}

const auto context = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
constexpr auto mesh_id = AssetId{0x6d617961, 0x5}, material_id = AssetId{0x6d617961, 0x70};

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
float unit(uint64_t seed) { return float(mix(seed) >> 40) / float(1u << 24); }

// A 64 m cell's content: scattered props on the ground.
SceneDocument make_cell(uint32_t count, uint32_t cell) {
    auto document = SceneDocument{};
    document.entities.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto seed = (uint64_t(cell) << 32) | i;
        auto transform = TransformComponent{};
        transform.translation = {unit(seed) * 64.0f, 0.0f, unit(seed ^ 0x55) * 64.0f};
        transform.rotation = math::Quat::from_axis_angle({0, 1, 0}, unit(seed ^ 0x77) * 6.28f);
        transform.scale = math::Vec3(0.8f + 0.4f * unit(seed ^ 0x99));
        auto renderer = MeshRendererComponent{};
        renderer.mesh = {mesh_id};
        renderer.material = {material_id};
        auto entity = SceneEntity{EntityId{0x63656c6c00000000ull | cell, i + 1}, std::nullopt, {transform, renderer}};
        if (i % 4 == 0) entity.components.push_back(ColliderComponent{});
        if (i % 10 == 0) entity.components.push_back(NameComponent{"Rock " + std::to_string(i)});
        document.entities.push_back(std::move(entity));
    }
    return document;
}

// --- A packed binary cook: fixed records, names in a string table. -----------------------------------
struct PackedEntity {
    uint64_t id_high, id_low;
    float translation[3], rotation[4], scale[3];
    uint64_t mesh_high, mesh_low, material_high, material_low;
    uint32_t flags; // 1: collider, 2: name
    uint32_t name_offset, name_size;
    float collider_half[3];
};
std::string cook(const SceneDocument& document) {
    auto records = std::vector<PackedEntity>{};
    auto names = std::string{};
    for (const auto& entity : document.entities) {
        auto packed = PackedEntity{};
        packed.id_high = entity.id.high, packed.id_low = entity.id.low;
        for (const auto& value : entity.components) {
            if (const auto* t = std::get_if<TransformComponent>(&value)) {
                std::memcpy(packed.translation, &t->translation, sizeof packed.translation);
                packed.rotation[0] = t->rotation.x, packed.rotation[1] = t->rotation.y, packed.rotation[2] = t->rotation.z, packed.rotation[3] = t->rotation.w;
                std::memcpy(packed.scale, &t->scale, sizeof packed.scale);
            } else if (const auto* r = std::get_if<MeshRendererComponent>(&value)) {
                packed.mesh_high = r->mesh.id.high, packed.mesh_low = r->mesh.id.low;
                packed.material_high = r->material.id.high, packed.material_low = r->material.id.low;
            } else if (const auto* c = std::get_if<ColliderComponent>(&value)) {
                packed.flags |= 1;
                std::memcpy(packed.collider_half, &c->half_extents, sizeof packed.collider_half);
            } else if (const auto* n = std::get_if<NameComponent>(&value)) {
                packed.flags |= 2;
                packed.name_offset = uint32_t(names.size()), packed.name_size = uint32_t(n->value.size());
                names += n->value;
            }
        }
        records.push_back(packed);
    }
    auto out = std::string(sizeof(uint32_t), '\0');
    const auto count = uint32_t(records.size());
    std::memcpy(out.data(), &count, sizeof count);
    out.append(reinterpret_cast<const char*>(records.data()), records.size() * sizeof(PackedEntity));
    out += names;
    return out;
}
SceneDocument decode(const std::string& bytes) {
    auto count = uint32_t{};
    std::memcpy(&count, bytes.data(), sizeof count);
    const auto* records = reinterpret_cast<const PackedEntity*>(bytes.data() + sizeof count);
    const auto* names = bytes.data() + sizeof count + count * sizeof(PackedEntity);
    auto document = SceneDocument{};
    document.entities.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& p = records[i];
        auto transform = TransformComponent{};
        std::memcpy(&transform.translation, p.translation, sizeof p.translation);
        transform.rotation = math::Quat(p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]);
        std::memcpy(&transform.scale, p.scale, sizeof p.scale);
        auto renderer = MeshRendererComponent{};
        renderer.mesh = {AssetId{p.mesh_high, p.mesh_low}};
        renderer.material = {AssetId{p.material_high, p.material_low}};
        auto entity = SceneEntity{EntityId{p.id_high, p.id_low}, std::nullopt, {transform, renderer}};
        if (p.flags & 1) {
            auto collider = ColliderComponent{};
            std::memcpy(&collider.half_extents, p.collider_half, sizeof p.collider_half);
            entity.components.push_back(collider);
        }
        if (p.flags & 2) entity.components.push_back(NameComponent{std::string(names + p.name_offset, p.name_size)});
        document.entities.push_back(std::move(entity));
    }
    return document;
}

std::string read_file(const std::filesystem::path& path) {
    auto in = std::ifstream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// --- Activation into a populated World. ---------------------------------------------------------------
std::unique_ptr<World> populated(uint32_t count) {
    auto world = std::make_unique<World>();
    auto commands = world->commands();
    for (uint32_t i = 0; i < count; ++i) {
        const auto e = commands.create(EntityId{0x706f7075, i + 1});
        commands.add(e, TransformComponent{{float(i % 1000), 0, float(i / 1000)}, {}, math::Vec3(1.0f)});
        commands.add(e, MeshRendererComponent{{mesh_id}, {material_id}, true});
    }
    if (!world->commit(commands)) unexpected("could not populate the World");
    return world;
}
// Adds a cell's entities `slice` at a time, one commit a slice, as bounded activation would across frames;
// returns the slowest slice. Then removes them the same way and reports the slowest removal.
std::pair<double, double> activate(World& world, SceneDocument document, uint32_t slice) {
    auto handles = std::vector<EntityId>{};
    auto slowest_add = 0.0, slowest_remove = 0.0;
    for (size_t first = 0; first < document.entities.size(); first += slice) {
        const auto last = std::min(document.entities.size(), first + slice);
        const auto start = Clock::now();
        auto commands = world.commands();
        for (auto i = first; i < last; ++i) {
            auto& entity = document.entities[i];
            const auto pending = commands.create(entity.id);
            for (auto& value : entity.components) std::visit([&](auto& component) { commands.add(pending, std::move(component)); }, value);
            handles.push_back(entity.id);
        }
        if (!world.commit(commands)) unexpected("an activation slice was rejected");
        slowest_add = std::max(slowest_add, ms_since(start));
    }
    for (size_t first = 0; first < handles.size(); first += slice) {
        const auto last = std::min(handles.size(), first + slice);
        const auto start = Clock::now();
        auto commands = world.commands();
        for (auto i = first; i < last; ++i)
            if (const auto handle = world.find(handles[i])) commands.destroy(*handle);
        if (!world.commit(commands)) unexpected("a removal slice was rejected");
        slowest_remove = std::max(slowest_remove, ms_since(start));
    }
    return {slowest_add, slowest_remove};
}

// --- Jolt: a cell's colliders joining and leaving a populated physics world. -------------------------
struct BodyTimes {
    double add_single, add_batch, remove_single, remove_batch;
};
BodyTimes body_times() {
    auto scene = prototype::PhysicsScene{};
    auto& bodies = scene.system.GetBodyInterface();
    const auto box = JPH::RefConst<JPH::Shape>(new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f)));
    const auto make = [&](int first, int count) {
        auto ids = std::vector<JPH::BodyID>{};
        for (int i = first; i < first + count; ++i)
            ids.push_back(bodies.CreateBody(JPH::BodyCreationSettings(box, JPH::RVec3(float(i % 300) * 2, 0.5f, float(i / 300) * 2),
                JPH::Quat::sIdentity(), JPH::EMotionType::Static, prototype::static_layer))->GetID());
        return ids;
    };
    auto world = make(0, 50000);
    auto state = bodies.AddBodiesPrepare(world.data(), int(world.size()));
    bodies.AddBodiesFinalize(world.data(), int(world.size()), state, JPH::EActivation::DontActivate);
    scene.system.OptimizeBroadPhase();

    auto times = BodyTimes{};
    auto cell = make(60000, 1000);
    auto start = Clock::now();
    for (const auto id : cell) bodies.AddBody(id, JPH::EActivation::DontActivate);
    times.add_single = ms_since(start);
    start = Clock::now();
    for (const auto id : cell) bodies.RemoveBody(id);
    times.remove_single = ms_since(start);
    start = Clock::now();
    state = bodies.AddBodiesPrepare(cell.data(), int(cell.size()));
    bodies.AddBodiesFinalize(cell.data(), int(cell.size()), state, JPH::EActivation::DontActivate);
    times.add_batch = ms_since(start);
    start = Clock::now();
    bodies.RemoveBodies(cell.data(), int(cell.size()));
    times.remove_batch = ms_since(start);
    bodies.DestroyBodies(cell.data(), int(cell.size()));
    bodies.RemoveBodies(world.data(), int(world.size()));
    bodies.DestroyBodies(world.data(), int(world.size()));
    return times;
}
} // namespace

int main(int argc, char** argv) {
    const auto folder = std::filesystem::path(argc > 1 ? argv[1] : (std::filesystem::temp_directory_path() / "maya-cells-prototype").string());
    std::filesystem::create_directories(folder);
    std::printf("Cell prototype (#1060)\n\n1. Format: one file per cell, text against a packed binary cook\n");
    std::printf("%9s %11s %11s %9s %11s %13s %12s %11s\n", "entities", "text KiB", "binary KiB", "file ms", "parse ms", "decode ms",
        "validate ms", "per entity");
    for (const auto count : {500u, 2000u, 8000u}) {
        const auto document = make_cell(count, count);
        auto out = std::ostringstream{};
        if (!write_scene(out, document, context).empty()) unexpected("the cell did not write");
        const auto text = out.str();
        const auto binary = cook(document);
        std::ofstream(folder / "cell.scene", std::ios::binary) << text;
        std::ofstream(folder / "cell.bin", std::ios::binary) << binary;
        const auto file = best_of(5, [&] { read_file(folder / "cell.bin"); });
        auto parsed = SceneDocumentResult{};
        const auto parse = best_of(5, [&] { parsed = read_scene(text, context); });
        if (!parsed) unexpected("the cell's text did not read back");
        auto decoded = SceneDocument{};
        const auto decode_ms = best_of(5, [&] { decoded = decode(binary); });
        const auto validate = best_of(5, [&] {
            auto copy = decoded;
            if (!validate_scene(copy, context).empty()) unexpected("the decoded cell did not validate");
        });
        if (decoded.entities.size() != parsed.document.entities.size()) unexpected("the binary cook lost entities");
        std::printf("%9u %11.1f %11.1f %9.3f %11.2f %13.2f %12.2f %8.2f us\n", count, double(text.size()) / 1024, double(binary.size()) / 1024,
            file, parse, decode_ms, validate, (decode_ms + validate) * 1000 / count);
    }

    std::printf("\n2. Activation: a 4,000-entity cell joining a World of 100,000, slice by slice, then leaving\n");
    std::printf("%11s %16s %16s %14s\n", "slice", "slowest add ms", "slowest rm ms", "add us/entity");
    {
        auto world = populated(100000);
        for (const auto slice : {250u, 1000u, 4000u}) {
            activate(*world, make_cell(4000, 7), slice); // warm the World's pools
            const auto [add, remove] = activate(*world, make_cell(4000, 7), slice);
            std::printf("%11u %16.3f %16.3f %14.2f\n", slice, add, remove, add * 1000 / slice);
            // The activation budget assumes about 2,000 entities in 1 ms of the owner thread.
            if (slice == 1000 && add > 1.0) unexpected("activating 1,000 entities took " + std::to_string(add) + " ms");
        }
    }
    std::printf("\n   a 1,000-entity commit against the size of the World it joins\n");
    auto by_size = std::vector<double>{};
    for (const auto size : {10000u, 100000u, 400000u}) {
        auto world = populated(size);
        activate(*world, make_cell(1000, 9), 1000);
        const auto [add, remove] = activate(*world, make_cell(1000, 9), 1000);
        by_size.push_back(add);
        std::printf("%11u entities: add %.3f ms, remove %.3f ms\n", size, add, remove);
    }
    if (by_size.back() > 3 * by_size.front()) unexpected("a commit's cost grows with the World's size: " + std::to_string(by_size.front()) + " ms at 10,000, " + std::to_string(by_size.back()) + " ms at 400,000");

    std::printf("\n3. Physics: 1,000 static colliders joining and leaving a Jolt world of 50,000\n");
    {
        auto jolt = prototype::JoltRuntime{};
        auto t = body_times();
        t = body_times();
        std::printf("   add one at a time %.3f ms, batched %.3f ms; remove one at a time %.3f ms, batched %.3f ms\n", t.add_single,
            t.add_batch, t.remove_single, t.remove_batch);
        if (t.add_batch > t.add_single || t.remove_batch > t.remove_single) unexpected("Jolt's batch calls were not faster than single calls");
    }
    std::filesystem::remove_all(folder);
    if (failures) std::printf("\n%d unexpected results\n", failures);
    return failures ? 1 : 0;
}
