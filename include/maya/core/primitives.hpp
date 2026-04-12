#pragma once

#include "maya/core/mesh.hpp"
#include "maya/rhi/graphics_device.hpp"
#include "maya/math/vector.hpp"
#include <memory>

namespace maya {

/// Axis-aligned cube centered at the origin with vertex colors (for unlit pipeline).
std::unique_ptr<Mesh> make_color_cube(GraphicsDevice& device, float half_extent,
    const math::Vec3& color_tint = math::Vec3(1.0f, 1.0f, 1.0f));

} // namespace maya
