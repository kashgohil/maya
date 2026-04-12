#include <catch2/catch_test_macros.hpp>
#include "maya/rhi/graphics_device.hpp"
#include "maya/rhi/resource.hpp"
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
