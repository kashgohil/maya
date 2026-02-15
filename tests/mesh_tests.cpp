#include <catch2/catch_test_macros.hpp>
#include "maya/core/mesh.hpp"
#include "maya/rhi/graphics_device.hpp"

using namespace maya;

// Mock GraphicsDevice for testing
class MockGraphicsDeviceForMesh : public GraphicsDevice {
public:
    bool initialize(void*) override { return true; }
    void shutdown() override {}
    void begin_frame() override {}
    void end_frame() override {}
    bool create_pipeline(const std::string&) override { return true; }
    
    VertexBufferHandle create_vertex_buffer(const void*, size_t size) override {
        last_vertex_buffer_size = size;
        return {next_handle++};
    }
    
    IndexBufferHandle create_index_buffer(const void*, size_t size) override {
        last_index_buffer_size = size;
        return {next_handle++};
    }
    
    UniformBufferHandle create_uniform_buffer(size_t) override { return {next_handle++}; }
    void update_uniform_buffer(UniformBufferHandle, const void*, size_t) override {}
    TextureHandle create_texture(const void*, uint32_t, uint32_t) override { return {next_handle++}; }
    void bind_vertex_buffer(VertexBufferHandle, uint32_t) override {}
    void bind_uniform_buffer(UniformBufferHandle, uint32_t) override {}
    void bind_texture(TextureHandle, uint32_t) override {}
    void draw_indexed(IndexBufferHandle, uint32_t count) override {
        last_draw_count = count;
    }
    
    size_t last_vertex_buffer_size = 0;
    size_t last_index_buffer_size = 0;
    uint32_t last_draw_count = 0;
    
private:
    uint32_t next_handle = 1;
};

// =============================================================================
// Mesh Construction Tests
// =============================================================================
TEST_CASE("Mesh construction", "[core][mesh]") {
    MockGraphicsDeviceForMesh device;

    SECTION("Create mesh with triangle") {
        std::vector<Vertex> vertices = {
            Vertex(math::Vec3(0, 0, 0), math::Vec3(0, 1, 0), math::Vec4(1, 0, 0, 1)),
            Vertex(math::Vec3(1, 0, 0), math::Vec3(0, 1, 0), math::Vec4(0, 1, 0, 1)),
            Vertex(math::Vec3(0.5f, 1, 0), math::Vec3(0, 1, 0), math::Vec4(0, 0, 1, 1))
        };
        
        std::vector<uint32_t> indices = {0, 1, 2};
        
        Mesh mesh(device, vertices, indices);
        
        // Verify buffer sizes
        CHECK(device.last_vertex_buffer_size == vertices.size() * sizeof(Vertex));
        CHECK(device.last_index_buffer_size == indices.size() * sizeof(uint32_t));
    }

    SECTION("Create mesh with quad") {
        std::vector<Vertex> vertices = {
            Vertex(math::Vec3(0, 0, 0), math::Vec3(0, 1, 0), math::Vec4(1, 0, 0, 1)),
            Vertex(math::Vec3(1, 0, 0), math::Vec3(0, 1, 0), math::Vec4(0, 1, 0, 1)),
            Vertex(math::Vec3(1, 1, 0), math::Vec3(0, 1, 0), math::Vec4(0, 0, 1, 1)),
            Vertex(math::Vec3(0, 1, 0), math::Vec3(0, 1, 0), math::Vec4(1, 1, 0, 1))
        };
        
        std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
        
        Mesh mesh(device, vertices, indices);
        
        CHECK(device.last_vertex_buffer_size == vertices.size() * sizeof(Vertex));
        CHECK(device.last_index_buffer_size == indices.size() * sizeof(uint32_t));
    }

    SECTION("Create mesh with many vertices") {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        
        // Create a simple grid
        for (int i = 0; i < 100; ++i) {
            vertices.emplace_back(
                math::Vec3(static_cast<float>(i), 0, 0),
                math::Vec3(0, 1, 0),
                math::Vec4(1, 0, 0, 1)
            );
        }
        
        for (uint32_t i = 0; i < 99; ++i) {
            indices.push_back(i);
            indices.push_back(i + 1);
        }
        
        Mesh mesh(device, vertices, indices);
        
        CHECK(device.last_vertex_buffer_size == vertices.size() * sizeof(Vertex));
        CHECK(device.last_index_buffer_size == indices.size() * sizeof(uint32_t));
    }
}

// =============================================================================
// Mesh Drawing Tests
// =============================================================================
TEST_CASE("Mesh drawing", "[core][mesh]") {
    MockGraphicsDeviceForMesh device;

    SECTION("Draw calls bind buffers and draws") {
        std::vector<Vertex> vertices = {
            Vertex(math::Vec3(0, 0, 0), math::Vec3(0, 1, 0), math::Vec4(1, 0, 0, 1)),
            Vertex(math::Vec3(1, 0, 0), math::Vec3(0, 1, 0), math::Vec4(0, 1, 0, 1)),
            Vertex(math::Vec3(0.5f, 1, 0), math::Vec3(0, 1, 0), math::Vec4(0, 0, 1, 1))
        };
        
        std::vector<uint32_t> indices = {0, 1, 2};
        
        Mesh mesh(device, vertices, indices);
        mesh.draw();
        
        // Verify draw was called with correct count
        CHECK(device.last_draw_count == 3);
    }

    SECTION("Draw multiple times") {
        std::vector<Vertex> vertices = {
            Vertex(math::Vec3(0, 0, 0), math::Vec3(0, 1, 0), math::Vec4(1, 0, 0, 1)),
            Vertex(math::Vec3(1, 0, 0), math::Vec3(0, 1, 0), math::Vec4(0, 1, 0, 1)),
            Vertex(math::Vec3(0.5f, 1, 0), math::Vec3(0, 1, 0), math::Vec4(0, 0, 1, 1))
        };
        
        std::vector<uint32_t> indices = {0, 1, 2};
        
        Mesh mesh(device, vertices, indices);
        
        // Draw multiple times
        for (int i = 0; i < 5; ++i) {
            mesh.draw();
            CHECK(device.last_draw_count == 3);
        }
    }
}

// =============================================================================
// Mesh Edge Cases
// =============================================================================
TEST_CASE("Mesh edge cases", "[core][mesh]") {
    MockGraphicsDeviceForMesh device;

    SECTION("Empty mesh") {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        
        // Should handle gracefully
        Mesh mesh(device, vertices, indices);
        mesh.draw();
        
        // Draw count should be 0
        CHECK(device.last_draw_count == 0);
    }

    SECTION("Single vertex") {
        std::vector<Vertex> vertices = {
            Vertex(math::Vec3(0, 0, 0), math::Vec3(0, 1, 0), math::Vec4(1, 0, 0, 1))
        };
        std::vector<uint32_t> indices = {0};
        
        Mesh mesh(device, vertices, indices);
        mesh.draw();
        
        CHECK(device.last_draw_count == 1);
    }

    SECTION("Maximum indices") {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        
        // Create maximum uint32_t indices (limited by practical concerns)
        // Just test with a large number
        for (int i = 0; i < 1000; ++i) {
            vertices.emplace_back(
                math::Vec3(static_cast<float>(i), 0, 0),
                math::Vec3(0, 1, 0),
                math::Vec4(1, 0, 0, 1)
            );
            indices.push_back(static_cast<uint32_t>(i));
        }
        
        Mesh mesh(device, vertices, indices);
        mesh.draw();
        
        CHECK(device.last_draw_count == 1000);
    }

    SECTION("Mesh with UVs") {
        std::vector<Vertex> vertices = {
            Vertex(math::Vec3(0, 0, 0), math::Vec3(0, 1, 0), math::Vec4(1, 0, 0, 1), math::Vec2(0, 0)),
            Vertex(math::Vec3(1, 0, 0), math::Vec3(0, 1, 0), math::Vec4(0, 1, 0, 1), math::Vec2(1, 0)),
            Vertex(math::Vec3(0.5f, 1, 0), math::Vec3(0, 1, 0), math::Vec4(0, 0, 1, 1), math::Vec2(0.5f, 1))
        };
        
        std::vector<uint32_t> indices = {0, 1, 2};
        
        Mesh mesh(device, vertices, indices);
        mesh.draw();
        
        CHECK(device.last_draw_count == 3);
    }
}

// =============================================================================
// Mesh with Different Vertex Configurations
// =============================================================================
TEST_CASE("Mesh vertex configurations", "[core][mesh]") {
    MockGraphicsDeviceForMesh device;

    SECTION("Vertices with different positions") {
        std::vector<Vertex> vertices = {
            Vertex(math::Vec3(-10, -10, -10), math::Vec3(0, 1, 0), math::Vec4(1, 0, 0, 1)),
            Vertex(math::Vec3(10, 0, 0), math::Vec3(0, 1, 0), math::Vec4(0, 1, 0, 1)),
            Vertex(math::Vec3(0, 10, 10), math::Vec3(0, 1, 0), math::Vec4(0, 0, 1, 1))
        };
        
        std::vector<uint32_t> indices = {0, 1, 2};
        
        Mesh mesh(device, vertices, indices);
        CHECK(device.last_vertex_buffer_size == 3 * sizeof(Vertex));
    }

    SECTION("Vertices with different normals") {
        std::vector<Vertex> vertices = {
            Vertex(math::Vec3(0, 0, 0), math::Vec3(1, 0, 0), math::Vec4(1, 0, 0, 1)),
            Vertex(math::Vec3(1, 0, 0), math::Vec3(0, 1, 0), math::Vec4(0, 1, 0, 1)),
            Vertex(math::Vec3(0.5f, 1, 0), math::Vec3(0, 0, 1), math::Vec4(0, 0, 1, 1))
        };
        
        std::vector<uint32_t> indices = {0, 1, 2};
        
        Mesh mesh(device, vertices, indices);
        CHECK(device.last_vertex_buffer_size == 3 * sizeof(Vertex));
    }

    SECTION("Vertices with different colors") {
        std::vector<Vertex> vertices = {
            Vertex(math::Vec3(0, 0, 0), math::Vec3(0, 1, 0), math::Vec4(1, 0, 0, 0.5f)),
            Vertex(math::Vec3(1, 0, 0), math::Vec3(0, 1, 0), math::Vec4(0, 1, 0, 0.75f)),
            Vertex(math::Vec3(0.5f, 1, 0), math::Vec3(0, 1, 0), math::Vec4(0, 0, 1, 1.0f))
        };
        
        std::vector<uint32_t> indices = {0, 1, 2};
        
        Mesh mesh(device, vertices, indices);
        CHECK(device.last_vertex_buffer_size == 3 * sizeof(Vertex));
    }
}
