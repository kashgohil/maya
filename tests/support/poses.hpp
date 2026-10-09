#pragma once

// Float poses for tests near the origin (#1065): the engine places things with world poses that keep
// their translation in double (math::Affine); tests that build poses as float matrices near the origin
// widen them here. Far from the origin, tests build Affine poses directly.

#include "maya/renderer/render_snapshot.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/world/spatial.hpp"

namespace maya::test {

/// The transform as a float matrix (its translation narrowed): for transforms near the origin.
inline math::Mat4 local_matrix(const TransformComponent& value) { return local_pose(value).matrix(); }

inline std::optional<CameraMatrices> camera_matrices(const CameraComponent& camera, const math::Mat4& pose, float aspect) {
    return maya::camera_matrices(camera, math::Affine::from_matrix(pose), aspect);
}

inline std::optional<RenderView> make_render_view(const CameraComponent& camera, const math::Mat4& pose, uint32_t width,
                                                  uint32_t height) {
    return maya::make_render_view(camera, math::Affine::from_matrix(pose), width, height);
}

/// Where a world point lands in a view's clip space: views are camera-relative (#1065), so the point is
/// moved to the camera first, in double.
inline math::Vec4 clip_of(const RenderView& view, const math::DVec3& point) {
    return view.matrices.view_projection * math::Vec4((point - view.position).to_float(), 1.0f);
}

/// The distances the origin-offset sweep covers (#1065): the origin, 100 m, 1 km, 10 km, W1's farthest
/// corner (11.6 km), and 100 km.
inline constexpr double offset_distances[] = {0.0, 100.0, 1000.0, 10000.0, 11600.0, 100000.0};
/// An offset `distance` from the origin, along a diagonal in x and z, as W1's cells are.
inline math::DVec3 offset_toward(double distance) { return {distance * 0.6, 0.0, -distance * 0.8}; }
/// Moves a scene by `offset`: every root entity's translation.
inline void shift_roots(SceneDocument& document, const math::DVec3& offset) {
    for (auto& entity : document.entities)
        if (!entity.parent)
            for (auto& component : entity.components)
                if (auto* transform = std::get_if<TransformComponent>(&component)) transform->translation += offset;
}

} // namespace maya::test
