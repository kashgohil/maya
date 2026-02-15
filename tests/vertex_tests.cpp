#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "maya/rhi/vertex.hpp"

using namespace maya;
using namespace maya::math;

// =============================================================================
// Vertex Struct Size and Alignment Tests
// =============================================================================
TEST_CASE("Vertex struct size", "[rhi][vertex]") {
    SECTION("Size is 64 bytes") {
        // Metal requires 16-byte alignment for float4
        // position: 12 bytes (Vec3) + 4 pad = 16
        // normal: 12 bytes (Vec3) + 4 pad = 16
        // color: 16 bytes (Vec4)
        // uv: 8 bytes (Vec2) + 8 pad = 16
        // Total: 64 bytes
        CHECK(sizeof(Vertex) == 64);
    }

    SECTION("Size matches Metal shader expectations") {
        // This is critical for GPU compatibility
        static_assert(sizeof(Vertex) == 64, "Vertex struct must be 64 bytes for Metal");
    }
}

TEST_CASE("Vertex struct alignment", "[rhi][vertex]") {
    SECTION("Alignment is natural alignment") {
        // Without explicit alignas, struct has natural alignment (largest member)
        // Vec4 has 4-byte alignment, so Vertex has 4-byte alignment
        CHECK(alignof(Vertex) == 4);
    }

    SECTION("Member offsets are correct") {
        Vertex v(Vec3(1,2,3), Vec3(4,5,6), Vec4(7,8,9,10), Vec2(11,12));
        
        // Verify position is at offset 0
        CHECK(reinterpret_cast<uintptr_t>(&v.position) - reinterpret_cast<uintptr_t>(&v) == 0);
        
        // Verify normal is at offset 16
        CHECK(reinterpret_cast<uintptr_t>(&v.normal) - reinterpret_cast<uintptr_t>(&v) == 16);
        
        // Verify color is at offset 32
        CHECK(reinterpret_cast<uintptr_t>(&v.color) - reinterpret_cast<uintptr_t>(&v) == 32);
        
        // Verify uv is at offset 48
        CHECK(reinterpret_cast<uintptr_t>(&v.uv) - reinterpret_cast<uintptr_t>(&v) == 48);
    }
}

// =============================================================================
// Vertex Construction Tests
// =============================================================================
TEST_CASE("Vertex construction", "[rhi][vertex]") {
    SECTION("Constructor with all parameters") {
        Vec3 pos(1.0f, 2.0f, 3.0f);
        Vec3 norm(0.0f, 1.0f, 0.0f);
        Vec4 col(1.0f, 0.0f, 0.0f, 1.0f);
        Vec2 uv_coord(0.5f, 0.5f);
        
        Vertex v(pos, norm, col, uv_coord);
        
        CHECK(v.position.x == 1.0f);
        CHECK(v.position.y == 2.0f);
        CHECK(v.position.z == 3.0f);
        
        CHECK(v.normal.x == 0.0f);
        CHECK(v.normal.y == 1.0f);
        CHECK(v.normal.z == 0.0f);
        
        CHECK(v.color.x == 1.0f);
        CHECK(v.color.y == 0.0f);
        CHECK(v.color.z == 0.0f);
        CHECK(v.color.w == 1.0f);
        
        CHECK(v.uv.x == 0.5f);
        CHECK(v.uv.y == 0.5f);
    }

    SECTION("Constructor with default UV") {
        Vec3 pos(1.0f, 2.0f, 3.0f);
        Vec3 norm(0.0f, 1.0f, 0.0f);
        Vec4 col(1.0f, 1.0f, 1.0f, 1.0f);
        
        Vertex v(pos, norm, col); // No UV provided
        
        // Default UV should be (0, 0)
        CHECK(v.uv.x == 0.0f);
        CHECK(v.uv.y == 0.0f);
    }

    SECTION("Padding is zero-initialized") {
        Vec3 pos(1.0f, 2.0f, 3.0f);
        Vec3 norm(0.0f, 1.0f, 0.0f);
        Vec4 col(1.0f, 0.0f, 0.0f, 1.0f);
        Vec2 uv_coord(0.5f, 0.5f);
        
        Vertex v(pos, norm, col, uv_coord);
        
        // Padding should be zero
        CHECK(v._pad0 == 0.0f);
        CHECK(v._pad1 == 0.0f);
        CHECK(v._pad2[0] == 0.0f);
        CHECK(v._pad2[1] == 0.0f);
    }
}

// =============================================================================
// Vertex Array Tests
// =============================================================================
TEST_CASE("Vertex array layout", "[rhi][vertex]") {
    SECTION("Vertices are contiguous in memory") {
        std::vector<Vertex> vertices;
        vertices.emplace_back(Vec3(0,0,0), Vec3(0,1,0), Vec4(1,0,0,1));
        vertices.emplace_back(Vec3(1,0,0), Vec3(0,1,0), Vec4(0,1,0,1));
        vertices.emplace_back(Vec3(0,1,0), Vec3(0,1,0), Vec4(0,0,1,1));
        
        // Verify size
        CHECK(vertices.size() * sizeof(Vertex) == 3 * 64);
        
        // Verify contiguity
        uintptr_t addr1 = reinterpret_cast<uintptr_t>(&vertices[0]);
        uintptr_t addr2 = reinterpret_cast<uintptr_t>(&vertices[1]);
        CHECK(addr2 - addr1 == 64);
    }

    SECTION("Vertex buffer offset calculation") {
        // Simulate how Metal would calculate offsets
        size_t vertex_size = sizeof(Vertex);
        
        // Attribute offsets within vertex
        size_t position_offset = offsetof(Vertex, position);
        size_t normal_offset = offsetof(Vertex, normal);
        size_t color_offset = offsetof(Vertex, color);
        size_t uv_offset = offsetof(Vertex, uv);
        
        CHECK(position_offset == 0);
        CHECK(normal_offset == 16);
        CHECK(color_offset == 32);
        CHECK(uv_offset == 48);
        
        // Total size matches
        CHECK(vertex_size == 64);
    }
}

// =============================================================================
// Vertex Edge Cases
// =============================================================================
TEST_CASE("Vertex edge cases", "[rhi][vertex]") {
    SECTION("Zero position") {
        Vertex v(Vec3(0,0,0), Vec3(0,1,0), Vec4(1,0,0,1));
        CHECK(v.position.x == 0.0f);
        CHECK(v.position.y == 0.0f);
        CHECK(v.position.z == 0.0f);
    }

    SECTION("Zero normal") {
        Vertex v(Vec3(1,2,3), Vec3(0,0,0), Vec4(1,0,0,1));
        CHECK(v.normal.x == 0.0f);
        CHECK(v.normal.y == 0.0f);
        CHECK(v.normal.z == 0.0f);
    }

    SECTION("Transparent color") {
        Vertex v(Vec3(1,2,3), Vec3(0,1,0), Vec4(1,0,0,0.5f));
        CHECK(v.color.w == 0.5f);
    }

    SECTION("UV coordinates out of 0-1 range") {
        // UV can be outside 0-1 (for tiling)
        Vertex v(Vec3(1,2,3), Vec3(0,1,0), Vec4(1,0,0,1), Vec2(2.5f, -0.5f));
        CHECK(v.uv.x == 2.5f);
        CHECK(v.uv.y == -0.5f);
    }

    SECTION("Large coordinate values") {
        Vertex v(Vec3(10000.0f, 20000.0f, 30000.0f), 
                 Vec3(0,1,0), 
                 Vec4(1,0,0,1));
        CHECK(v.position.x == 10000.0f);
        CHECK(v.position.y == 20000.0f);
        CHECK(v.position.z == 30000.0f);
    }

    SECTION("Small coordinate values") {
        float small = 0.0001f;
        Vertex v(Vec3(small, small, small), 
                 Vec3(0,1,0), 
                 Vec4(1,0,0,1));
        CHECK(v.position.x == small);
        CHECK(v.position.y == small);
        CHECK(v.position.z == small);
    }

    SECTION("Negative coordinates") {
        Vertex v(Vec3(-1.0f, -2.0f, -3.0f), 
                 Vec3(0,1,0), 
                 Vec4(1,0,0,1));
        CHECK(v.position.x == -1.0f);
        CHECK(v.position.y == -2.0f);
        CHECK(v.position.z == -3.0f);
    }
}

// =============================================================================
// Vertex Comparison Tests (for testing purposes)
// =============================================================================
TEST_CASE("Vertex equality", "[rhi][vertex]") {
    SECTION("Identical vertices") {
        Vertex v1(Vec3(1,2,3), Vec3(0,1,0), Vec4(1,0,0,1), Vec2(0.5,0.5));
        Vertex v2(Vec3(1,2,3), Vec3(0,1,0), Vec4(1,0,0,1), Vec2(0.5,0.5));
        
        CHECK(v1.position.x == v2.position.x);
        CHECK(v1.position.y == v2.position.y);
        CHECK(v1.position.z == v2.position.z);
        CHECK(v1.normal.x == v2.normal.x);
        CHECK(v1.color.x == v2.color.x);
        CHECK(v1.uv.x == v2.uv.x);
    }

    SECTION("Different vertices") {
        Vertex v1(Vec3(1,2,3), Vec3(0,1,0), Vec4(1,0,0,1));
        Vertex v2(Vec3(4,5,6), Vec3(0,1,0), Vec4(1,0,0,1));
        
        CHECK(v1.position.x != v2.position.x);
    }
}

// =============================================================================
// Vertex Memory Layout Validation
// =============================================================================
TEST_CASE("Vertex memory layout", "[rhi][vertex]") {
    SECTION("Layout matches Metal shader expectations") {
        // Metal shader expects:
        //   packed_float3 position;
        //   packed_float3 normal;
        //   float4 color;
        //   float2 uv;
        //
        // With std140 alignment rules:
        //   position: offset 0, size 12 (but aligned to 16)
        //   normal: offset 16, size 12 (but aligned to 16)
        //   color: offset 32, size 16
        //   uv: offset 48, size 8 (padded to 16)
        
        // Verify our offsets match
        CHECK(offsetof(Vertex, position) == 0);
        CHECK(offsetof(Vertex, normal) == 16);
        CHECK(offsetof(Vertex, color) == 32);
        CHECK(offsetof(Vertex, uv) == 48);
        
        // Verify total size
        CHECK(sizeof(Vertex) == 64);
    }

    SECTION("Can be used in contiguous buffer") {
        // Create a mock vertex buffer
        std::vector<Vertex> buffer;
        buffer.reserve(100);
        
        for (int i = 0; i < 100; ++i) {
            buffer.emplace_back(
                Vec3(static_cast<float>(i), 0, 0),
                Vec3(0, 1, 0),
                Vec4(1, 0, 0, 1),
                Vec2(static_cast<float>(i) / 100.0f, 0)
            );
        }
        
        // Verify buffer size
        CHECK(buffer.size() == 100);
        CHECK(buffer.size() * sizeof(Vertex) == 100 * 64);
        
        // Verify we can access all vertices
        for (int i = 0; i < 100; ++i) {
            CHECK(buffer[i].position.x == static_cast<float>(i));
        }
    }
}
