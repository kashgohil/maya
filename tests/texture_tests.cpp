#include <catch2/catch_test_macros.hpp>
#include "maya/core/texture.hpp"
#include "maya/rhi/graphics_device.hpp"

using namespace maya;

// Mock GraphicsDevice for testing
class MockGraphicsDeviceForTexture : public GraphicsDevice {
public:
    bool initialize(void*) override { return true; }
    void shutdown() override {}
    void begin_frame() override {}
    void end_frame() override {}
    PipelineHandle create_pipeline(const std::string&, const std::string&, const std::string&) override {
        return {next_pipeline++};
    }
    VertexBufferHandle create_vertex_buffer(const void*, size_t) override { return {1}; }
    IndexBufferHandle create_index_buffer(const void*, size_t) override { return {2}; }
    UniformBufferHandle create_uniform_buffer(size_t) override { return {3}; }
    void update_uniform_buffer(UniformBufferHandle, const void*, size_t) override {}
    
    TextureHandle create_texture(const void* data, uint32_t w, uint32_t h) override {
        last_texture_data = data;
        last_texture_width = w;
        last_texture_height = h;
        return {next_handle++};
    }
    
    void bind_vertex_buffer(VertexBufferHandle, uint32_t) override {}
    void bind_uniform_buffer(UniformBufferHandle, uint32_t) override {}
    
    void bind_texture(TextureHandle, uint32_t slot) override {
        last_bind_slot = slot;
    }
    
    void draw_indexed(IndexBufferHandle, uint32_t) override {}
    
    const void* last_texture_data = nullptr;
    uint32_t last_texture_width = 0;
    uint32_t last_texture_height = 0;
    uint32_t last_bind_slot = 0;
    
private:
    uint32_t next_handle = 1;
    uint32_t next_pipeline = 1;
};

// =============================================================================
// Texture Construction Tests
// =============================================================================
TEST_CASE("Texture construction", "[core][texture]") {
    MockGraphicsDeviceForTexture device;

    SECTION("Create texture with RGBA data") {
        uint32_t pixels[] = {
            0xFF0000FF, 0x00FF00FF,
            0x0000FFFF, 0xFFFFFFFF
        };
        
        Texture texture(device, pixels, 2, 2);
        
        CHECK(device.last_texture_data == pixels);
        CHECK(device.last_texture_width == 2);
        CHECK(device.last_texture_height == 2);
    }

    SECTION("Create texture with 1x1 data") {
        uint32_t pixel = 0xFF00FF00;
        
        Texture texture(device, &pixel, 1, 1);
        
        CHECK(device.last_texture_width == 1);
        CHECK(device.last_texture_height == 1);
    }

    SECTION("Create texture with large dimensions") {
        std::vector<uint32_t> pixels(1024 * 1024, 0xFFFFFFFF);
        
        Texture texture(device, pixels.data(), 1024, 1024);
        
        CHECK(device.last_texture_width == 1024);
        CHECK(device.last_texture_height == 1024);
    }

    SECTION("Create texture with non-square dimensions") {
        std::vector<uint32_t> pixels(100 * 200, 0xFF0000FF);
        
        Texture texture(device, pixels.data(), 100, 200);
        
        CHECK(device.last_texture_width == 100);
        CHECK(device.last_texture_height == 200);
    }

    SECTION("Create texture with wide aspect") {
        std::vector<uint32_t> pixels(1920 * 1080, 0xFFFFFFFF);
        
        Texture texture(device, pixels.data(), 1920, 1080);
        
        CHECK(device.last_texture_width == 1920);
        CHECK(device.last_texture_height == 1080);
    }
}

// =============================================================================
// Texture Binding Tests
// =============================================================================
TEST_CASE("Texture binding", "[core][texture]") {
    MockGraphicsDeviceForTexture device;

    SECTION("Bind to slot 0") {
        uint32_t pixel = 0xFF00FF00;
        Texture texture(device, &pixel, 1, 1);
        
        texture.bind(0);
        
        CHECK(device.last_bind_slot == 0);
    }

    SECTION("Bind to slot 1") {
        uint32_t pixel = 0xFF00FF00;
        Texture texture(device, &pixel, 1, 1);
        
        texture.bind(1);
        
        CHECK(device.last_bind_slot == 1);
    }

    SECTION("Bind to higher slot") {
        uint32_t pixel = 0xFF00FF00;
        Texture texture(device, &pixel, 1, 1);
        
        texture.bind(7);
        
        CHECK(device.last_bind_slot == 7);
    }

    SECTION("Bind default slot (0)") {
        uint32_t pixel = 0xFF00FF00;
        Texture texture(device, &pixel, 1, 1);
        
        texture.bind(); // Default slot
        
        CHECK(device.last_bind_slot == 0);
    }

    SECTION("Multiple binds") {
        uint32_t pixel = 0xFF00FF00;
        Texture texture(device, &pixel, 1, 1);
        
        texture.bind(0);
        CHECK(device.last_bind_slot == 0);
        
        texture.bind(1);
        CHECK(device.last_bind_slot == 1);
        
        texture.bind(2);
        CHECK(device.last_bind_slot == 2);
    }
}

// =============================================================================
// Texture Edge Cases
// =============================================================================
TEST_CASE("Texture edge cases", "[core][texture]") {
    MockGraphicsDeviceForTexture device;

    SECTION("Null data pointer") {
        // Should handle gracefully (device implementation dependent)
        Texture texture(device, nullptr, 1, 1);
        CHECK(device.last_texture_data == nullptr);
    }

    SECTION("Zero dimensions") {
        uint32_t pixel = 0xFF00FF00;
        
        // Should handle gracefully
        Texture texture(device, &pixel, 0, 0);
        
        CHECK(device.last_texture_width == 0);
        CHECK(device.last_texture_height == 0);
    }

    SECTION("Very small texture") {
        uint32_t pixel = 0xFF00FF00;
        Texture texture(device, &pixel, 1, 1);
        
        CHECK(device.last_texture_width == 1);
        CHECK(device.last_texture_height == 1);
    }

    SECTION("Different pixel values") {
        uint32_t red = 0xFF0000FF;
        uint32_t green = 0x00FF00FF;
        uint32_t blue = 0x0000FFFF;
        uint32_t white = 0xFFFFFFFF;
        uint32_t transparent = 0x00000000;
        
        Texture tex1(device, &red, 1, 1);
        Texture tex2(device, &green, 1, 1);
        Texture tex3(device, &blue, 1, 1);
        Texture tex4(device, &white, 1, 1);
        Texture tex5(device, &transparent, 1, 1);
        
        // All should create successfully
        CHECK(true);
    }

    SECTION("Bind to max slot") {
        uint32_t pixel = 0xFF00FF00;
        Texture texture(device, &pixel, 1, 1);
        
        // Metal typically supports up to 16 texture slots
        texture.bind(15);
        CHECK(device.last_bind_slot == 15);
    }
}

// =============================================================================
// Texture Multiple Instances
// =============================================================================
TEST_CASE("Texture multiple instances", "[core][texture]") {
    MockGraphicsDeviceForTexture device;

    SECTION("Multiple textures") {
        uint32_t red = 0xFF0000FF;
        uint32_t green = 0x00FF00FF;
        uint32_t blue = 0x0000FFFF;
        
        Texture tex1(device, &red, 1, 1);
        Texture tex2(device, &green, 1, 1);
        Texture tex3(device, &blue, 1, 1);
        
        tex1.bind(0);
        CHECK(device.last_bind_slot == 0);
        
        tex2.bind(1);
        CHECK(device.last_bind_slot == 1);
        
        tex3.bind(2);
        CHECK(device.last_bind_slot == 2);
    }

    SECTION("Different size textures") {
        uint32_t small = 0xFF0000FF;
        std::vector<uint32_t> large(100 * 100, 0x00FF00FF);
        
        Texture tex_small(device, &small, 1, 1);
        Texture tex_large(device, large.data(), 100, 100);
        
        CHECK(device.last_texture_width == 100);
        CHECK(device.last_texture_height == 100);
    }
}
