#include <catch2/catch_test_macros.hpp>
#include "maya/rhi/graphics_device.hpp"
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
        
        CHECK(device->create_pipeline(valid_shader) == true);
        
        std::string invalid_shader = "this is not a shader";
        CHECK(device->create_pipeline(invalid_shader) == false);
        
        device->shutdown();
    }
}
