#include "maya/core/primitives.hpp"
#include "maya/rhi/vertex.hpp"
#include <array>
#include <cmath>
#include <vector>

namespace maya {

std::unique_ptr<Mesh> make_color_cube(GraphicsDevice& device, float half_extent,
    const math::Vec3& color_tint) {
    const float h = half_extent;
    const std::array<math::Vec3, 8> pos = {{
        {-h, -h, -h},
        {h, -h, -h},
        {h, h, -h},
        {-h, h, -h},
        {-h, -h, h},
        {h, -h, h},
        {h, h, h},
        {-h, h, h},
    }};

    std::vector<Vertex> vertices;
    vertices.reserve(8);
    for (const auto& p : pos) {
        math::Vec3 n = math::Vec3(p.x, p.y, p.z).normalized();
        const float r = color_tint.x * (0.35f + 0.65f * std::fabs(p.x / h));
        const float g = color_tint.y * (0.35f + 0.65f * std::fabs(p.y / h));
        const float b = color_tint.z * (0.35f + 0.65f * std::fabs(p.z / h));
        vertices.emplace_back(p, n, math::Vec4(r, g, b, 1.0f), math::Vec2(0.0f, 0.0f));
    }

    // CCW faces as seen from outside (matches back-face cull + CCW front face).
    const std::array<uint32_t, 36> indices = {{
        0, 3, 1, 1, 3, 2,
        4, 5, 6, 4, 6, 7,
        0, 4, 3, 3, 4, 7,
        1, 2, 5, 5, 2, 6,
        0, 1, 4, 1, 5, 4,
        3, 7, 2, 2, 7, 6,
    }};

    return std::make_unique<Mesh>(device, vertices,
        std::vector<uint32_t>(indices.begin(), indices.end()));
}

} // namespace maya
