#include <catch2/catch_test_macros.hpp>
#include "maya/core/engine.hpp"
#include "maya/platform/window.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "maya/assets/registry.hpp"
#include "maya/core/file_system.hpp"

namespace {
class FailingApplication final : public maya::Application {
public:
    bool on_start(maya::GraphicsDevice& device) override {
        // Acquire a real GPU resource before failing to exercise rollback.
        device.create_uniform_buffer(256);
        return false;
    }
};
}

TEST_CASE("Desktop sessions survive partial startup and repeated shutdown", "[desktop]") {
    maya::Window window(320, 240, "Maya lifecycle test");
    REQUIRE(window.get_native_handle() != nullptr);
    maya::Engine engine;
    CHECK_FALSE(engine.initialize(maya::GraphicsDevice::create_default(),
        window.get_native_handle(), std::make_unique<FailingApplication>()));
    for (int session = 0; session < 3; ++session) {
        REQUIRE(engine.initialize(maya::GraphicsDevice::create_default(),
            window.get_native_handle(), std::make_unique<maya::Application>()));
        REQUIRE(engine.tick(1.0f / 60.0f, false));
        engine.shutdown();
        engine.shutdown();
    }
}

TEST_CASE("Failed and overlapping windows do not invalidate surviving windows", "[desktop]") {
    for (int session = 0; session < 2; ++session) {
        maya::Window first(320, 240, "Maya window lifecycle test");
        REQUIRE(first.get_native_handle() != nullptr);
        {
            maya::Window invalid(0, 0, "Invalid window");
            CHECK(invalid.get_native_handle() == nullptr);
            CHECK(invalid.should_close());
        }
        {
            maya::Window second(320, 240, "Maya second window");
            REQUIRE(second.get_native_handle() != nullptr);
        }
        first.poll_events();
        CHECK(first.get_native_handle() != nullptr);
        CHECK_FALSE(first.should_close());
        CHECK(first.framebuffer_size().first > 0);
    }
}

TEST_CASE("Metal keeps encoded mesh resources alive after the final asset lease is released", "[desktop][assets]") {
    maya::Window window(320,240,"Maya asset retirement test");
    REQUIRE(window.get_native_handle());
    maya::MetalDevice device;
    REQUIRE(device.initialize(window.get_native_handle()));
    const auto source=maya::FileSystem::resolve("samples/basic_scene/assets/pyramid.obj");
    REQUIRE(source);
    maya::AssetRegistry registry(source->parent_path(),std::make_unique<maya::FileAssetProvider>(device));
    const auto reference=maya::AssetRef<maya::MeshAsset>{{0x6d617961,1}};
    REQUIRE_FALSE(registry.register_asset(reference,"pyramid.obj"));
    const auto pipeline=device.create_pipeline(R"(
        #include <metal_stdlib>
        using namespace metal;
        vertex float4 vertexMain(uint id [[vertex_id]]) {
            const float2 positions[3] = {float2(-.5,-.5),float2(.5,-.5),float2(0,.5)};
            return float4(positions[id%3],0.5,1);
        }
        fragment float4 fragmentMain() { return float4(1,0,1,1); }
    )");
    REQUIRE(pipeline.handle != maya::INVALID_HANDLE);
    for (int frame=0;frame<3;++frame) {
        auto asset=registry.acquire(reference); REQUIRE(asset);
        device.begin_frame(); device.bind_pipeline(pipeline);
        asset.lease.value().mesh().draw();
        asset={}; CHECK(registry.evict_unused() == 1);
        CHECK(device.resident_buffer_count() == 0); // encoded Metal work still owns native resources
        device.end_frame();
    }
    device.shutdown(); // drains submitted work before destroying the device
}
