#include <catch2/catch_test_macros.hpp>
#include "maya/rhi/graphics_device.hpp"
#include "maya/rhi/resource.hpp"
#include "maya/rhi/metal/metal_device.hpp"
#include "maya/assets/registry.hpp"
#include "maya/core/file_system.hpp"
#include <string>

using namespace maya;

TEST_CASE("RHI GraphicsDevice initialization", "[rhi]") {
    auto device = GraphicsDevice::create_default();
    
    SECTION("Device initialization") {
        // Test headless initialization
        bool success = device->initialize(nullptr);
        CHECK(success == true);
        device->shutdown();
    }

    SECTION("Shader compilation") {
        device->initialize(nullptr);
        
        std::string valid_shader = R"(
            #include <metal_stdlib>
            using namespace metal;
            vertex float4 vertexMain(uint vid [[vertex_id]]) { return float4(0); }
            fragment float4 fragmentMain() { return float4(1); }
        )";
        
        PipelineHandle good = device->create_pipeline(valid_shader);
        CHECK(good.handle != INVALID_HANDLE);

        std::string invalid_shader = "this is not a shader";
        PipelineHandle bad = device->create_pipeline(invalid_shader);
        CHECK(bad.handle == INVALID_HANDLE);
        
        device->shutdown();
    }

    SECTION("Resize is safe without a Metal layer (headless)") {
        device->initialize(nullptr);
        device->resize(1920, 1080);
        device->shutdown();
    }
}

TEST_CASE("Metal asset sharing and buffer retirement survive repeated sessions", "[rhi][assets]") {
    MetalDevice device;
    const auto source = FileSystem::resolve("samples/basic_scene/assets/pyramid.obj");
    REQUIRE(source);
    const auto reference = AssetRef<MeshAsset>{{0x6d617961,1}};
    for (int session=0; session<3; ++session) {
        REQUIRE(device.initialize(nullptr));
        AssetRegistry registry(source->parent_path(),std::make_unique<FileAssetProvider>(device));
        REQUIRE_FALSE(registry.register_asset(reference,"pyramid.obj"));
        auto first=registry.acquire(reference), second=registry.acquire(reference);
        REQUIRE(first); REQUIRE(second);
        CHECK(&first.lease.value() == &second.lease.value());
        CHECK(device.resident_buffer_count() == 2);
        first={}; CHECK(registry.evict_unused() == 0);
        second={}; CHECK(registry.evict_unused() == 1);
        CHECK(device.resident_buffer_count() == 0);
        auto held=registry.acquire(reference); REQUIRE(held);
        device.shutdown();
        CHECK(device.resident_buffer_count() == 0);
        CHECK_FALSE(held.lease.value().mesh().valid());
        CHECK_FALSE(registry.resolve(held.lease.handle()));
    }
}
