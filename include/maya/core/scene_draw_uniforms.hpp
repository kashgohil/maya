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
    math::Vec4 camera_position_world;
    /// RGB = specular reflectance tint; w = Blinn–Phong shininess exponent.
    math::Vec4 specular_rgb_shininess;
};

static_assert(sizeof(SceneDrawUniforms) == 208, "SceneDrawUniforms must match Metal constant layout");
static_assert(offsetof(SceneDrawUniforms, model_matrix) == 0);
static_assert(offsetof(SceneDrawUniforms, view_projection_matrix) == 64);
static_assert(offsetof(SceneDrawUniforms, light_dir_world) == 128);
static_assert(offsetof(SceneDrawUniforms, ambient_rgb) == 144);
static_assert(offsetof(SceneDrawUniforms, light_diffuse_rgb) == 160);
static_assert(offsetof(SceneDrawUniforms, camera_position_world) == 176);
static_assert(offsetof(SceneDrawUniforms, specular_rgb_shininess) == 192);

/// Direction from a surface point toward the light (world space, unit length recommended).
struct DirectionalLighting {
    math::Vec3 direction_to_light;
    math::Vec3 ambient;
    math::Vec3 diffuse;
    math::Vec3 specular;
    float shininess;

    static DirectionalLighting default_sun() {
        DirectionalLighting L{};
        L.direction_to_light = math::Vec3(0.4f, 0.85f, 0.35f).normalized();
        L.ambient = {0.06f, 0.07f, 0.09f};
        L.diffuse = {1.0f, 0.97f, 0.9f};
        L.specular = {1.0f, 1.0f, 1.0f};
        L.shininess = 48.0f;
        return L;
    }
};

} // namespace maya
