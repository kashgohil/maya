#include "maya/assets/registry.hpp"
#include "maya/rhi/null_device.hpp"
#include "maya/world/world.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_set>

namespace {
using namespace maya;
struct Counters {
    size_t uploads=0, releases=0, draws=0;
    std::unordered_set<uint32_t> application; // native buffers owned by the backend
};
/// Null backend that counts buffer uploads/releases. Frames complete only when told to.
class CountingDevice final : public NullGraphicsDevice {
public:
    std::shared_ptr<Counters> counts = std::make_shared<Counters>();
    bool fail_index=false, throw_index=false;
    // No per-frame upload memory, so buffer counts reflect only the assets under test.
    CountingDevice() : NullGraphicsDevice({false,0,0,true}) { REQUIRE(initialize(nullptr,{3,0})); }
    ~CountingDevice() override { shutdown(); }
    /// Begin a frame with an offscreen pass and pipeline so meshes can encode draws.
    void open_pass() {
        if (!m_target.valid() || !describe(m_target)) {
            m_target=create_texture({4,4,Format::rgba8_unorm,TextureUsage::render_target,"target"}).handle;
            auto pipeline=PipelineDesc{}; pipeline.shader_source="test"; pipeline.color_formats={Format::rgba8_unorm};
            m_pipeline=create_pipeline(pipeline).handle;
        }
        REQUIRE_FALSE(begin_frame());
        auto pass=RenderPassDesc{}; pass.colors.push_back({m_target});
        REQUIRE_FALSE(begin_render_pass(pass)); REQUIRE_FALSE(set_pipeline(m_pipeline));
    }
    uint64_t close_frame() {
        REQUIRE_FALSE(end_render_pass()); REQUIRE_FALSE(end_frame());
        return stats().submitted_frames;
    }
protected:
    RhiDiagnostic backend_create_buffer(uint32_t slot,const BufferDesc& desc,const void* data) override {
        if (has_flag(desc.usage,BufferUsage::index)) {
            if (throw_index) throw std::runtime_error("injected index upload failure");
            if (fail_index) return {RhiError::out_of_memory,"injected index upload failure"};
        }
        counts->application.insert(slot); ++counts->uploads;
        return NullGraphicsDevice::backend_create_buffer(slot,desc,data);
    }
    void backend_release(ResourceKind kind,uint32_t slot) noexcept override {
        if (kind==ResourceKind::buffer && counts->application.erase(slot)) ++counts->releases;
        NullGraphicsDevice::backend_release(kind,slot);
    }
    void backend_draw_indexed(uint32_t slot,IndexType type,uint32_t count,size_t offset,uint32_t instances) override {
        ++counts->draws;
        NullGraphicsDevice::backend_draw_indexed(slot,type,count,offset,instances);
    }
private:
    TextureHandle m_target;
    PipelineHandle m_pipeline;
};
struct ProviderCounters {
    size_t meshes=0, materials=0;
    bool throw_load=false, bad_alloc=false;
    std::function<void()> on_load;
};
class CountingProvider final : public AssetProvider {
public:
    CountingProvider(GraphicsDevice& device,std::shared_ptr<ProviderCounters> counts)
        : files(device),counts(std::move(counts)) {}
    AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path& path) override {
        ++counts->meshes;
        if (counts->on_load) counts->on_load();
        if (counts->bad_alloc) throw std::bad_alloc{};
        if (counts->throw_load) throw std::runtime_error("injected provider failure");
        return files.load_mesh(path);
    }
    AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path& path) override {
        ++counts->materials; return files.load_material(path);
    }
private:
    FileAssetProvider files;
    std::shared_ptr<ProviderCounters> counts;
};
struct Project {
    std::filesystem::path root = std::filesystem::temp_directory_path()/
        ("maya-assets-"+std::to_string(detail::next_lifetime_token()));
    Project() {
        std::filesystem::create_directories(root);
        write("triangle.obj","v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        write("surface.mat","maya-material 1\nbase_color 0.2 0.4 0.8 1\nmetallic 0.25\nroughness 0.5\n");
    }
    ~Project() { std::error_code error; std::filesystem::remove_all(root,error); }
    void write(const std::string& path,const std::string& text) { std::ofstream(root/path) << text; }
};
const AssetRef<MeshAsset> mesh_ref{{1,1}};
const AssetRef<MaterialAsset> material_ref{{1,2}};
static_assert(!std::is_convertible_v<AssetHandle<MeshAsset>,AssetHandle<MaterialAsset>>);
static_assert(!std::is_copy_constructible_v<Mesh> && !std::is_move_constructible_v<Mesh>);

TEST_CASE("Assets deduplicate source loads and GPU buffers across worlds and entities", "[assets]") {
    Project project; CountingDevice device;
    auto counts=std::make_shared<ProviderCounters>();
    AssetRegistry registry(project.root,std::make_unique<CountingProvider>(device,counts));
    REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
    REQUIRE_FALSE(registry.register_asset(material_ref,"surface.mat"));
    CHECK(registry.info(mesh_ref.id)->state == AssetState::unloaded);
    auto mesh=registry.acquire(mesh_ref), second=registry.acquire(mesh_ref);
    REQUIRE(mesh); REQUIRE(second);
    CHECK(&mesh.lease.value() == &second.lease.value());
    CHECK(mesh.lease.handle() == second.lease.handle());
    CHECK(counts->meshes == 1); CHECK(device.counts->uploads == 2);
    auto material=registry.acquire(material_ref); REQUIRE(material);
    CHECK(material.lease.value().roughness == .5f);
    CHECK(registry.acquire(material_ref)); CHECK(counts->materials == 1);
    CHECK(registry.info(mesh_ref.id)->state == AssetState::ready);
    struct ResidentMesh { AssetLease<MeshAsset> lease; };
    World first, other;
    for (World& world : {std::ref(first),std::ref(other)}) {
        auto commands=world.commands();
        for (int i=0;i<100;++i) {
            const auto entity=commands.create();
            commands.add(entity,MeshRendererComponent{mesh_ref,material_ref});
            commands.add(entity,ResidentMesh{registry.acquire(mesh_ref).lease});
        }
        REQUIRE(world.commit(commands));
    }
    mesh={}; second={}; material={};
    CHECK(registry.evict_unused() == 1); // only material is unpinned
    CHECK(device.counts->application.size() == 2);
    auto remove=first.commands(); first.for_each_entity([&](auto entity){remove.destroy(entity);});
    REQUIRE(first.commit(remove)); CHECK(registry.evict_unused() == 0);
    remove=other.commands(); other.for_each_entity([&](auto entity){remove.destroy(entity);});
    REQUIRE(other.commit(remove)); CHECK(registry.evict_unused() == 1);
    CHECK(device.counts->application.empty()); CHECK(device.counts->releases == 2);
}

TEST_CASE("Eviction invalidates handles and reload acquires a fresh generation", "[assets]") {
    Project project; CountingDevice device;
    AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
    auto loaded=registry.acquire(mesh_ref); REQUIRE(loaded);
    const auto handle=loaded.lease.handle();
    CHECK(registry.resolve(handle)); CHECK(registry.evict_unused() == 0);
    loaded={}; CHECK(registry.evict_unused() == 1);
    CHECK(registry.resolve(handle).diagnostic.code == AssetError::stale_handle);
    CHECK(registry.info(mesh_ref.id)->state == AssetState::unloaded);
    loaded=registry.acquire(mesh_ref); REQUIRE(loaded);
    CHECK(loaded.lease.handle().generation != handle.generation);
    CHECK_FALSE(registry.resolve(handle));
    AssetRegistry foreign(project.root,std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(foreign.register_asset(mesh_ref,"triangle.obj"));
    CHECK_FALSE(foreign.resolve(loaded.lease.handle()));
    const auto h=loaded.lease.handle();
    CHECK(registry.resolve(AssetHandle<MaterialAsset>{h.registry,h.slot,h.generation}).diagnostic.code == AssetError::wrong_type);
}

TEST_CASE("Reload preserves old leases and failed reload preserves the published version", "[assets]") {
    Project project; CountingDevice device;
    AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
    auto original=registry.acquire(mesh_ref); REQUIRE(original);
    const auto handle=original.lease.handle();
    project.write("triangle.obj","v invalid\nf 1 2 3\n");
    auto failed=registry.reload(mesh_ref); CHECK_FALSE(failed);
    CHECK_FALSE(failed.diagnostic.message.empty());
    CHECK(registry.info(mesh_ref.id)->state == AssetState::ready);
    CHECK(registry.info(mesh_ref.id)->diagnostic);
    CHECK(registry.acquire(mesh_ref).lease.handle() == handle);
    CHECK(registry.resolve(handle)); CHECK(device.counts->uploads == 2);
    project.write("triangle.obj","v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n");
    auto replacement=registry.reload(mesh_ref); REQUIRE(replacement);
    CHECK(&replacement.lease.value() != &original.lease.value());
    CHECK_FALSE(registry.resolve(handle)); CHECK(original.lease.value().mesh().valid());
    CHECK(device.counts->application.size() == 4);
    CHECK_FALSE(registry.info(mesh_ref.id)->diagnostic);
    original={}; CHECK(device.counts->application.size() == 2);
    replacement={}; CHECK(registry.evict_unused() == 1); CHECK(device.counts->application.empty());
}

TEST_CASE("Missing assets retain identity diagnostics and explicit retry", "[assets]") {
    Project project; CountingDevice device;
    AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(registry.register_asset(mesh_ref,"missing.obj"));
    const auto missing=registry.acquire(mesh_ref);
    CHECK_FALSE(missing); CHECK(missing.diagnostic.code == AssetError::missing_file);
    CHECK(missing.diagnostic.message.find("missing.obj") != std::string::npos);
    CHECK(registry.info(mesh_ref.id)->record.id == mesh_ref.id);
    CHECK(registry.info(mesh_ref.id)->state == AssetState::failed);
    project.write("missing.obj","v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    CHECK_FALSE(registry.acquire(mesh_ref)); // no retry storm on each frame
    CHECK(registry.reload(mesh_ref));
    CHECK(registry.acquire(AssetRef<MeshAsset>{{9,9}}).diagnostic.code == AssetError::not_registered);
    CHECK(registry.acquire(AssetRef<MaterialAsset>{mesh_ref.id}).diagnostic.code == AssetError::wrong_type);
    CHECK(fallback_material().base_color.x == 1); CHECK(fallback_material().base_color.y == 0);
}

TEST_CASE("Catalog round trip preserves IDs across registry and world lifetimes", "[assets]") {
    Project project; CountingDevice device;
    AssetHandle<MeshAsset> old_handle;
    auto catalog=std::stringstream{};
    {
        AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
        REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
        REQUIRE_FALSE(registry.register_asset(material_ref,"surface.mat"));
        old_handle=registry.acquire(mesh_ref).lease.handle();
        write_asset_catalog(catalog,registry.records());
    }
    CHECK(device.counts->application.empty());
    const auto parsed=read_asset_catalog(catalog); REQUIRE(parsed);
    REQUIRE(parsed.records.size() == 2); CHECK(parsed.records[0].id == mesh_ref.id);
    AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
    for (const auto& record:parsed.records) REQUIRE_FALSE(registry.register_asset(record));
    auto reconstructed=registry.acquire(mesh_ref); REQUIRE(reconstructed);
    CHECK_FALSE(registry.resolve(old_handle));
    CHECK(reconstructed.lease.reference() == mesh_ref);
    World world;
    auto commands=world.commands(); const auto entity=commands.create(EntityId{7,8});
    commands.add(entity,MeshRendererComponent{mesh_ref,material_ref}); REQUIRE(world.commit(commands));
    world.for_each<MeshRendererComponent>([&](auto,const auto& mesh) {
        CHECK(&registry.acquire(mesh.mesh).lease.value() == &reconstructed.lease.value());
    });
}

TEST_CASE("Material reload preserves immutable factors and project relocation preserves references", "[assets]") {
    Project original, relocated; CountingDevice device;
    AssetRegistry registry(original.root,std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(registry.register_asset(material_ref,"surface.mat"));
    auto material=registry.acquire(material_ref); REQUIRE(material);
    const auto handle=material.lease.handle();
    original.write("surface.mat","maya-material 1 base_color 1 1 1 1 metallic 0 roughness 0.1");
    auto replacement=registry.reload(material_ref); REQUIRE(replacement);
    CHECK(material.lease.value().roughness == .5f);
    CHECK(replacement.lease.value().roughness == .1f);
    CHECK_FALSE(registry.resolve(handle));
    original.write("surface.mat","broken material");
    CHECK_FALSE(registry.reload(material_ref));
    CHECK(registry.resolve(replacement.lease.handle()));
    auto catalog=std::stringstream{}; write_asset_catalog(catalog,registry.records());
    const auto parsed=read_asset_catalog(catalog); REQUIRE(parsed);
    AssetRegistry moved(relocated.root,std::make_unique<FileAssetProvider>(device));
    for (const auto& record:parsed.records) REQUIRE_FALSE(moved.register_asset(record));
    auto loaded=moved.acquire(material_ref); REQUIRE(loaded);
    CHECK(loaded.lease.reference() == material_ref);
    CHECK(loaded.lease.value().roughness == .5f); // from the relocated project's source
    CHECK(device.counts->uploads == 0);
}

TEST_CASE("Registry rejects duplicate identity path aliases and escaped project paths", "[assets]") {
    Project project; CountingDevice device;
    AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(registry.register_asset(mesh_ref,"./triangle.obj"));
    CHECK(registry.register_asset(mesh_ref,"different.obj").code == AssetError::duplicate_id);
    CHECK(registry.register_asset(AssetRef<MeshAsset>{{2,2}},"nested/../triangle.obj").code == AssetError::duplicate_path);
    CHECK(registry.register_asset(AssetRef<MeshAsset>{},"other.obj").code == AssetError::invalid_id);
    CHECK(registry.register_asset(AssetRef<MeshAsset>{{2,2}},"../outside.obj").code == AssetError::invalid_path);
    CHECK(registry.register_asset(AssetRef<MeshAsset>{{2,2}},project.root/"triangle.obj").code == AssetError::invalid_path);
    CHECK(registry.register_asset(AssetRef<MeshAsset>{{2,2}},"").code == AssetError::invalid_path);
    std::filesystem::create_symlink(project.root.parent_path(),project.root/"escape");
    CHECK(registry.register_asset(AssetRef<MeshAsset>{{2,2}},"escape/outside.obj").code == AssetError::invalid_path);
    REQUIRE_FALSE(registry.register_asset(AssetRef<MeshAsset>{{3,3}},"late/outside.obj"));
    std::filesystem::create_symlink(project.root.parent_path(),project.root/"late");
    CHECK(registry.acquire(AssetRef<MeshAsset>{{3,3}}).diagnostic.code == AssetError::invalid_path);
}

TEST_CASE("Provider loading state exception recovery and mutation exclusion", "[assets]") {
    Project project; CountingDevice device;
    auto counts=std::make_shared<ProviderCounters>();
    AssetRegistry registry(project.root,std::make_unique<CountingProvider>(device,counts));
    REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
    counts->on_load=[&] {
        CHECK(registry.info(mesh_ref.id)->state == AssetState::loading);
        CHECK(registry.acquire(mesh_ref).diagnostic.code == AssetError::busy);
        CHECK(registry.register_asset(material_ref,"surface.mat").code == AssetError::busy);
        CHECK(registry.evict_unused() == 0);
    };
    counts->bad_alloc=true;
    CHECK_THROWS_AS(registry.acquire(mesh_ref),std::bad_alloc);
    CHECK(registry.info(mesh_ref.id)->state == AssetState::unloaded);
    counts->bad_alloc=false; counts->throw_load=true;
    CHECK(registry.acquire(mesh_ref).diagnostic.code == AssetError::load_failed);
    counts->throw_load=false;
    CHECK(registry.reload(mesh_ref));
}

TEST_CASE("Mesh partial upload rolls back buffers on failure and exceptions", "[assets]") {
    Project project; CountingDevice device;
    AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
    SECTION("invalid allocation handle") { device.fail_index=true; }
    SECTION("throwing allocation") { device.throw_index=true; }
    CHECK_FALSE(registry.acquire(mesh_ref));
    CHECK(device.counts->uploads == 1); CHECK(device.counts->releases == 1);
    CHECK(device.counts->application.empty());
}

TEST_CASE("Leases survive registry teardown and release safely after device destruction", "[assets]") {
    Project project;
    AssetLease<MeshAsset> lease;
    auto counts=std::shared_ptr<Counters>{};
    {
        CountingDevice device; counts=device.counts;
        {
            AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
            REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
            lease=registry.acquire(mesh_ref).lease; REQUIRE(lease);
        }
        CHECK(lease.value().mesh().valid()); CHECK(counts->application.size() == 2);
        device.open_pass();
        CHECK_FALSE(lease.value().mesh().draw()); CHECK(counts->draws == 1);
        // Device destruction abandons the frame and expires the guard even without explicit shutdown.
    }
    CHECK_FALSE(lease.value().mesh().valid());
    CHECK(counts->application.empty()); CHECK(counts->releases == 2); // released by device shutdown
    CHECK(lease.value().mesh().draw().code == RhiError::stale_handle); CHECK(counts->draws == 1);
    lease={}; CHECK(counts->releases == 2); // no call through a dead device
}

TEST_CASE("Final CPU release and submitted GPU retention have separate ownership", "[assets]") {
    Project project; CountingDevice device;
    {
        AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
        REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
        auto mesh=registry.acquire(mesh_ref); REQUIRE(mesh);
        device.open_pass();
        CHECK_FALSE(mesh.lease.value().mesh().draw());
        mesh={}; CHECK(registry.evict_unused() == 1);
        // Handles are revoked at once, but the encoding frame still owns the native buffers.
        CHECK(device.stats().buffers == 0);
        CHECK(device.stats().pending_retirements == 2);
        CHECK(device.counts->application.size() == 2);
    }
    const auto frame=device.close_frame();
    CHECK(device.counts->application.size() == 2); // submitted, not yet complete
    device.complete_through(frame); REQUIRE_FALSE(device.begin_frame()); // retirement runs at frame boundaries
    CHECK(device.counts->application.empty()); CHECK(device.stats().pending_retirements == 0);
    REQUIRE_FALSE(device.end_frame());
}

TEST_CASE("Device session changes invalidate old mesh resources and providers", "[assets]") {
    Project project; CountingDevice device;
    AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
    auto mesh=registry.acquire(mesh_ref); REQUIRE(mesh);
    device.shutdown(); REQUIRE(device.initialize(nullptr,{3,0}));
    CHECK_FALSE(mesh.lease.value().mesh().valid()); CHECK_FALSE(registry.resolve(mesh.lease.handle()));
    CHECK(registry.reload(mesh_ref).diagnostic.code == AssetError::device_unavailable);
    mesh={}; registry.evict_unused(); CHECK(device.counts->application.empty());
}

TEST_CASE("Repeated registry and world teardown releases every loaded buffer", "[assets]") {
    Project project; CountingDevice device;
    for (int iteration=0;iteration<25;++iteration) {
        {
            AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
            REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
            World world;
            auto commands=world.commands(); const auto entity=commands.create();
            commands.add(entity,registry.acquire(mesh_ref).lease); REQUIRE(world.commit(commands));
            CHECK(registry.evict_unused() == 0);
        }
        CHECK(device.counts->application.empty()); CHECK(device.counts->uploads == device.counts->releases);
    }
}

TEST_CASE("Malformed sources report errors before allocating GPU buffers", "[assets]") {
    Project project; CountingDevice device;
    AssetRegistry registry(project.root,std::make_unique<FileAssetProvider>(device));
    REQUIRE_FALSE(registry.register_asset(mesh_ref,"triangle.obj"));
    for (const auto& text : {"v\nf 1 2 3\n", "v 0 0 0\nf nope 1 1\n", "v 0 0 0\nf 1 1\n",
        "v 0 0 0\nf 1 1 1 1\n", "v 0 0 0\nf 1/9 1/9 1/9\n", "v 0 0 0\nf 99999999999999999999 1 1\n"}) {
        project.write("triangle.obj",text);
        const auto loaded=registry.reload(mesh_ref); CHECK_FALSE(loaded);
        CHECK(loaded.diagnostic.message.find("triangle.obj:") != std::string::npos);
        CHECK(device.counts->uploads == 0);
    }
    REQUIRE_FALSE(registry.register_asset(material_ref,"surface.mat"));
    project.write("surface.mat","maya-material 1\nbase_color 2 0 0 1\nmetallic 0\nroughness 1\n");
    CHECK(registry.acquire(material_ref).diagnostic.code == AssetError::invalid_data);
    for (const auto& text : {"", "maya-assets 2", "maya-assets 1\nmesh 1", "maya-assets 1\ntexture 1 2 foo", "maya-assets 1\nmesh -1 2 foo", "maya-assets 1\nmesh 1g 2 foo", "maya-assets 1\nmesh 0 0 foo"}) {
        auto input=std::istringstream(text); CHECK_FALSE(read_asset_catalog(input));
    }
}
} // namespace
