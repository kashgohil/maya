#pragma once

#include "maya/math/matrix.hpp"
#include "maya/math/vector.hpp"
#include <cstddef>

namespace maya {

/// GPU layout for `Scene::render` (must match `Uniforms` in `triangle.metal`).
struct SceneDrawUniforms {
    math::Mat4 model_matrix;
    math::Mat4 view_projection_matrix;
    math::Vec4 light_dir_world;
    math::Vec4 ambient_rgb;
    math::Vec4 light_diffuse_rgb;
};

static_assert(sizeof(SceneDrawUniforms) == 176, "SceneDrawUniforms must match Metal constant layout");
static_assert(offsetof(SceneDrawUniforms, model_matrix) == 0);
static_assert(offsetof(SceneDrawUniforms, view_projection_matrix) == 64);
static_assert(offsetof(SceneDrawUniforms, light_dir_world) == 128);
static_assert(offsetof(SceneDrawUniforms, ambient_rgb) == 144);
static_assert(offsetof(SceneDrawUniforms, light_diffuse_rgb) == 160);

/// Direction from a surface point toward the light (world space, unit length recommended).
struct DirectionalLighting {
    math::Vec3 direction_to_light;
    math::Vec3 ambient;
    math::Vec3 diffuse;

    static DirectionalLighting default_sun() {
        DirectionalLighting L{};
        L.direction_to_light = math::Vec3(0.4f, 0.85f, 0.35f).normalized();
        L.ambient = {0.06f, 0.07f, 0.09f};
        L.diffuse = {1.0f, 0.97f, 0.9f};
        return L;
    }
};

} // namespace maya
