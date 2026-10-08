// Asynchronous asset loading (#1062, docs/assets.md#asynchronous-loading): requests, merging, cancellation,
// failure, request trees, reloads in flight, late completions, budgets, explicit waits, and shutdown with
// loads in flight. A gated provider holds preparation until a test lets it go.

#include "maya/assets/registry.hpp"
#include "maya/rhi/null_device.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

using namespace maya;

namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool open = true;
    std::set<std::string> held; // file names held at the gate even while it is open
    std::set<std::string> failing; // file names whose preparation fails
    std::map<std::string, JobTier> tiers; // the tier each file last prepared on
    std::optional<JobTier> tier(const std::string& name) {
        auto lock = std::lock_guard(mutex);
        const auto found = tiers.find(name);
        return found == tiers.end() ? std::nullopt : std::optional(found->second);
    }
    std::atomic<int> prepared{0}, finalized{0}, waiting{0};
    std::atomic<int> stopped{0}; // preparations that saw their load cancelled after the gate
    void close() {
        auto lock = std::lock_guard(mutex);
        open = false;
    }
    void release() {
        {
            auto lock = std::lock_guard(mutex);
            open = true;
            held.clear();
        }
        changed.notify_all();
    }
    /// Until `count` preparations are held at the gate; false after a deadline, so a mistake fails, not hangs.
    [[nodiscard]] bool wait_for_waiting(int count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (waiting.load() < count && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return waiting.load() >= count;
    }
};

const AssetRef<MaterialAsset> plain{{1, 1}};
const AssetRef<MaterialAsset> textured{{1, 2}}; // names textures {1, 10} and {1, 11}
const AssetRef<TextureAsset> color{{1, 10}}, normal{{1, 11}};
const AssetRef<ScriptAsset> script{{1, 20}};

/// Prepares on the job thread, held at the gate; finalizes materials, scripts, and textures (the null
/// device's placeholder) on the owner thread.
class GatedProvider final : public AssetProvider {
public:
    GatedProvider(GraphicsDevice& device, std::shared_ptr<Gate> gate) : m_device(device), m_gate(std::move(gate)) {}
    PreparedAsset prepare(const AssetLoadRequest& request) override {
        const auto name = request.path.filename().string();
        ++m_gate->waiting;
        {
            auto lock = std::unique_lock(m_gate->mutex);
            m_gate->tiers[name] = request.tier;
            m_gate->changed.wait(lock, [&] { return m_gate->open && !m_gate->held.contains(name); });
        }
        --m_gate->waiting;
        ++m_gate->prepared;
        if (request.stopped()) {
            ++m_gate->stopped;
            return {{AssetError::cancelled, name + " was cancelled"}, 0, {}};
        }
        if (m_gate->failing.contains(name)) return {{AssetError::invalid_data, name + " is damaged"}, 0, {}};
        return {{}, size_t{1} << 20, [this, request, name]() -> std::pair<AssetValue, AssetDiagnostic> {
                    ++m_gate->finalized;
                    if (request.kind == AssetKind::texture) return {make_placeholder_texture(m_device), {}};
                    if (request.kind == AssetKind::script) return {std::make_shared<const ScriptAsset>(ScriptAsset{"-- " + name}), {}};
                    auto material = MaterialAsset{};
                    if (name == "textured.mat") {
                        material.base_color_texture = color;
                        material.normal_texture = normal;
                    }
                    return {std::make_shared<const MaterialAsset>(material), {}};
                }};
    }
    AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path&) override { return {}; }
    AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path&) override { return {}; }

private:
    GraphicsDevice& m_device;
    std::shared_ptr<Gate> m_gate;
};

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("maya-loading-" + std::to_string(detail::next_lifetime_token()));
    NullGraphicsDevice device;
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::unique_ptr<AssetRegistry> registry;
    Fixture() {
        std::filesystem::create_directories(root);
        for (const auto* file : {"plain.mat", "textured.mat", "color.png", "normal.png", "logic.luau"}) std::ofstream(root / file) << "x";
        REQUIRE(device.initialize(nullptr));
        registry = std::make_unique<AssetRegistry>(root, std::make_unique<GatedProvider>(device, gate));
        REQUIRE_FALSE(registry->register_asset(plain, "plain.mat"));
        REQUIRE_FALSE(registry->register_asset(textured, "textured.mat"));
        REQUIRE_FALSE(registry->register_asset(color, "color.png"));
        REQUIRE_FALSE(registry->register_asset(normal, "normal.png"));
        REQUIRE_FALSE(registry->register_asset(script, "logic.luau"));
    }
    ~Fixture() {
        gate->release();
        registry.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    /// Updates until `done` or a deadline; returns whether it became true.
    template<class F> bool update_until(F done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            registry->update();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return done();
    }
};
} // namespace

TEST_CASE("A request returns at once, prepares on a job, and becomes ready when an update finalizes it", "[assets][loading]") {
    auto f = Fixture{};
    f.gate->close();
    const auto request = f.registry->request(plain);
    CHECK(request.state() == AssetState::loading);
    CHECK(f.registry->info(plain.id)->state == AssetState::loading);
    REQUIRE(f.gate->wait_for_waiting(1)); // preparing on a worker, not here
    CHECK(f.gate->finalized == 0);
    f.gate->release();
    REQUIRE(f.update_until([&] { return request.done(); }));
    CHECK(request.state() == AssetState::ready);
    CHECK(f.registry->info(plain.id)->state == AssetState::ready);
    CHECK(f.gate->prepared == 1);
    CHECK(f.gate->finalized == 1);
    const auto stats = f.registry->load_stats();
    CHECK(stats.started == 1);
    CHECK(stats.finalized == 1);
    CHECK(stats.in_flight == 0);
    CHECK(stats.latency_max_ms > 0.0);
    // Resident now: a new request is ready at once, and try_acquire leases it without loading again.
    CHECK(f.registry->request(plain).state() == AssetState::ready);
    CHECK(f.registry->try_acquire(plain));
    CHECK(f.gate->prepared == 1);
}

TEST_CASE("try_acquire never blocks: it starts a load the registry keeps, and reports loading", "[assets][loading]") {
    auto f = Fixture{};
    f.gate->close();
    const auto first = f.registry->try_acquire(script);
    CHECK_FALSE(first);
    CHECK(first.diagnostic.code == AssetError::loading);
    REQUIRE(f.gate->wait_for_waiting(1));
    // No request is held, yet an update does not cancel it: the registry keeps loads try_acquire starts.
    f.registry->update();
    CHECK(f.registry->load_stats().cancelled == 0);
    f.gate->release();
    REQUIRE(f.update_until([&] { return bool(f.registry->try_acquire(script)); }));
    CHECK(f.registry->try_acquire(script).lease.value().source == "-- logic.luau");
}

TEST_CASE("A material loaded by try_acquire starts its maps' loads when it is ready, not when it is next drawn", "[assets][loading]") {
    auto f = Fixture{};
    CHECK_FALSE(f.registry->try_acquire(textured));
    REQUIRE(f.update_until([&] { return f.registry->info(textured.id)->state == AssetState::ready; }));
    // Ready, its maps already on their way, though no one asked for them yet; they are kept like it.
    CHECK(f.registry->info(color.id)->state != AssetState::unloaded);
    CHECK(f.registry->info(normal.id)->state != AssetState::unloaded);
    REQUIRE(f.update_until([&] { return f.registry->info(color.id)->state == AssetState::ready && f.registry->info(normal.id)->state == AssetState::ready; }));
    CHECK(f.registry->load_stats().cancelled == 0);
    CHECK(f.gate->tier("color.png") == JobTier::background);
}

TEST_CASE("A material an explicit wait loads starts its maps on the frame tier, where the wait for them cooks", "[assets][loading]") {
    auto f = Fixture{};
    const auto wait = AssetRegistry::ExplicitWait(*f.registry);
    REQUIRE(f.registry->acquire(textured));
    CHECK(f.gate->tier("textured.mat") == JobTier::frame);
    REQUIRE(f.registry->acquire(color)); // joins the load the material started
    f.registry->wait_idle();
    CHECK(f.gate->tier("color.png") == JobTier::frame);
    CHECK(f.gate->tier("normal.png") == JobTier::frame);
    CHECK(f.gate->prepared == 3); // the maps once each
}

TEST_CASE("Requests for an asset in flight merge into one load", "[assets][loading]") {
    auto f = Fixture{};
    f.gate->close();
    const auto a = f.registry->request(plain);
    const auto b = f.registry->request(plain);
    CHECK(f.registry->try_acquire(plain).diagnostic.code == AssetError::loading);
    f.gate->release();
    REQUIRE(f.update_until([&] { return a.done() && b.done(); }));
    CHECK(a.state() == AssetState::ready);
    CHECK(b.state() == AssetState::ready);
    CHECK(f.gate->prepared == 1);
    CHECK(f.registry->load_stats().merged == 2);
}

TEST_CASE("Dropping or cancelling the last request cancels the load, and nothing becomes resident", "[assets][loading]") {
    auto f = Fixture{};
    f.gate->close();
    SECTION("dropped") {
        { const auto request = f.registry->request(plain); }
        REQUIRE(f.gate->wait_for_waiting(1));
        f.registry->update(); // no interest left: cancelled
    }
    SECTION("cancelled") {
        const auto request = f.registry->request(plain);
        REQUIRE(f.gate->wait_for_waiting(1));
        request.cancel();
        f.registry->update();
        CHECK(request.state() == AssetState::failed);
        CHECK(request.diagnostic().code == AssetError::cancelled);
    }
    CHECK(f.registry->load_stats().cancelled == 1);
    CHECK(f.registry->info(plain.id)->state == AssetState::unloaded);
    f.gate->release(); // the preparation sees its cancellation between steps, stops, and posts nothing
    REQUIRE(f.update_until([&] { return f.gate->prepared == 1; }));
    CHECK(f.gate->stopped == 1);
    for (int i = 0; i < 5; ++i) f.registry->update();
    CHECK(f.gate->finalized == 0);
    CHECK(f.registry->info(plain.id)->state == AssetState::unloaded);
    CHECK(f.registry->residency().materials == 0);
    // Another request loads it afresh.
    const auto again = f.registry->request(plain);
    REQUIRE(f.update_until([&] { return again.done(); }));
    CHECK(again.state() == AssetState::ready);
}

TEST_CASE("A failed preparation reaches the requester, and a reload recovers", "[assets][loading]") {
    auto f = Fixture{};
    f.gate->failing.insert("plain.mat");
    const auto request = f.registry->request(plain);
    REQUIRE(f.update_until([&] { return request.done(); }));
    CHECK(request.state() == AssetState::failed);
    CHECK(request.diagnostic().message == "plain.mat is damaged");
    CHECK(f.registry->info(plain.id)->state == AssetState::failed);
    // A failed asset is not loaded again until it is reloaded.
    CHECK(f.registry->try_acquire(plain).diagnostic.message == "plain.mat is damaged");
    CHECK(f.registry->request(plain).state() == AssetState::failed);
    CHECK(f.gate->prepared == 1);
    f.gate->failing.clear();
    const auto reloaded = f.registry->request_reload(plain);
    REQUIRE(f.update_until([&] { return reloaded.done(); }));
    CHECK(reloaded.state() == AssetState::ready);
    CHECK(f.registry->try_acquire(plain));
    // A failed reload keeps the version in use.
    f.gate->failing.insert("plain.mat");
    const auto generation = f.registry->info(plain.id)->generation;
    const auto broken = f.registry->request_reload(plain);
    REQUIRE(f.update_until([&] { return broken.done(); }));
    CHECK(broken.state() == AssetState::failed);
    CHECK(f.registry->info(plain.id)->state == AssetState::ready);
    CHECK(f.registry->info(plain.id)->generation == generation);
    CHECK(f.registry->try_acquire(plain));
}

TEST_CASE("A material's request includes its textures: the tree is ready, fails, or is cancelled as one", "[assets][loading]") {
    auto f = Fixture{};
    SECTION("ready when every part is") {
        const auto request = f.registry->request(textured);
        REQUIRE(f.update_until([&] { return request.done(); }));
        CHECK(request.state() == AssetState::ready);
        CHECK(f.registry->info(color.id)->state == AssetState::ready);
        CHECK(f.registry->info(normal.id)->state == AssetState::ready);
        CHECK(f.gate->finalized == 3);
        // Resident: a new request of the tree is ready at once.
        CHECK(f.registry->request(textured).state() == AssetState::ready);
    }
    SECTION("failed when a part fails, though the material itself loaded") {
        f.gate->failing.insert("normal.png");
        const auto request = f.registry->request(textured);
        REQUIRE(f.update_until([&] { return request.done(); }));
        CHECK(request.state() == AssetState::failed);
        CHECK(request.diagnostic().message == "normal.png is damaged");
        CHECK(f.registry->info(textured.id)->state == AssetState::ready);
        CHECK(f.registry->info(color.id)->state == AssetState::ready);
    }
    SECTION("cancelled with its parts") {
        f.gate->held = {"color.png", "normal.png"}; // the textures are held mid-preparation
        const auto request = f.registry->request(textured);
        REQUIRE(f.update_until([&] { return f.registry->info(textured.id)->state == AssetState::ready; }));
        REQUIRE(f.gate->wait_for_waiting(2));
        request.cancel();
        f.registry->update();
        CHECK(request.state() == AssetState::failed);
        CHECK(f.registry->load_stats().cancelled == 2);
        f.gate->release();
        REQUIRE(f.update_until([&] { return f.gate->prepared == 3; }));
        for (int i = 0; i < 5; ++i) f.registry->update();
        CHECK(f.gate->finalized == 1); // the material alone
        CHECK(f.registry->info(color.id)->state == AssetState::unloaded);
        CHECK(f.registry->info(normal.id)->state == AssetState::unloaded);
    }
}

TEST_CASE("A reload while a load is in flight supersedes it; a late completion is discarded", "[assets][loading]") {
    auto f = Fixture{};
    SECTION("superseded while preparing: the old job sees its cancellation and posts nothing") {
        f.gate->close();
        const auto load = f.registry->request(plain);
        REQUIRE(f.gate->wait_for_waiting(1));
        const auto reload = f.registry->request_reload(plain); // the file changed while it was being read
        f.gate->release();
        REQUIRE(f.update_until([&] { return load.done() && reload.done(); }));
        CHECK(load.state() == AssetState::ready); // the first request follows the new load
        CHECK(reload.state() == AssetState::ready);
        CHECK(f.gate->prepared == 2);
    }
    SECTION("superseded after posting: the old completion arrives late and is discarded") {
        const auto load = f.registry->request(plain);
        while (f.gate->prepared < 1) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // its completion is queued, not yet applied
        const auto reload = f.registry->request_reload(plain);
        REQUIRE(f.update_until([&] { return reload.done(); }));
        CHECK(f.registry->load_stats().discarded == 1);
        CHECK(load.state() == AssetState::ready);
    }
    CHECK(f.gate->finalized == 1);
    CHECK(f.registry->info(plain.id)->generation == 1);
}

TEST_CASE("An update finalizes within its budget, at least one load each time", "[assets][loading]") {
    auto f = Fixture{};
    f.gate->close();
    const auto a = f.registry->request(plain), b = f.registry->request(script), c = f.registry->request(color);
    REQUIRE(f.gate->wait_for_waiting(3));
    f.gate->release();
    while (f.gate->prepared < 3) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // all three completions queued
    // A zero budget still finalizes one.
    auto stats = f.registry->update({std::chrono::microseconds(0), 0});
    CHECK(stats.last_finalized == 1);
    CHECK(stats.prepared == 2);
    // 1 MiB each (the provider says so): a 1.5 MiB budget fits one.
    stats = f.registry->update({std::chrono::microseconds(1000000), size_t{3} << 19});
    CHECK(stats.last_finalized == 1);
    CHECK(stats.last_bytes == size_t{1} << 20);
    stats = f.registry->update();
    CHECK(stats.last_finalized == 1);
    CHECK((a.done() && b.done() && c.done()));
    CHECK(f.registry->load_stats().finalized == 3);
}

TEST_CASE("acquire is an explicit wait: it finishes a load in flight, and a frame counts it unless declared", "[assets][loading]") {
    auto f = Fixture{};
    f.gate->close();
    const auto request = f.registry->request(plain); // background, held at the gate
    REQUIRE(f.gate->wait_for_waiting(1));
    auto opener = std::thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        f.gate->release();
    });
    f.registry->begin_frame();
    const auto waited = f.registry->acquire(plain); // the owner thread blocks: inside a frame, undeclared
    opener.join();
    REQUIRE(waited);
    CHECK(request.state() == AssetState::ready);
    CHECK(f.registry->load_stats().waited == 1);
    CHECK(f.registry->load_stats().waited_in_frames == 1);
    {
        const auto declared = AssetRegistry::ExplicitWait(*f.registry);
        CHECK(f.registry->acquire(script));
    }
    f.registry->end_frame();
    CHECK(f.registry->acquire(color)); // outside a frame
    CHECK(f.registry->load_stats().waited == 3);
    CHECK(f.registry->load_stats().waited_in_frames == 1);
    // A request tree waits as one.
    const auto tree = f.registry->request(textured);
    CHECK(f.registry->wait(tree) == AssetState::ready);
    f.registry->request(normal);
    f.registry->wait_idle();
    CHECK(f.registry->load_stats().in_flight == 0);
}

TEST_CASE("Destroying the registry with loads in flight cancels them and waits for their jobs", "[assets][loading]") {
    auto f = Fixture{};
    f.gate->close();
    auto requests = std::vector<AssetRequest>{};
    requests.push_back(f.registry->request(plain));
    requests.push_back(f.registry->request(textured));
    requests.push_back(f.registry->request(script));
    REQUIRE(f.gate->wait_for_waiting(1));
    auto opener = std::thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        f.gate->release(); // the jobs held at the gate finish into a registry that is going away
    });
    f.registry.reset();
    opener.join();
    CHECK(f.gate->finalized == 0);
    for (const auto& request : requests) CHECK(request.state() == AssetState::loading); // never settled
}
