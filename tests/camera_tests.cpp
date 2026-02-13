#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "maya/core/camera.hpp"

using namespace maya;

TEST_CASE("Camera basic functionality", "[core][camera]") {
    float fov = 60.0f;
    float aspect = 1.0f;
    float near = 0.1f;
    float far = 100.0f;
    Camera cam(fov, aspect, near, far);

    SECTION("Initial State") {
        math::Vec3 pos = cam.get_position();
        CHECK(pos.x == 0.0f);
        CHECK(pos.y == 0.0f);
        CHECK(pos.z == 3.0f);
    }

    SECTION("Set Position") {
        cam.set_position({10.0f, 5.0f, -2.0f});
        math::Vec3 pos = cam.get_position();
        CHECK(pos.x == 10.0f);
        CHECK(pos.y == 5.0f);
        CHECK(pos.z == -2.0f);
    }

    SECTION("Matrix Generation") {
        // Just verify it doesn't return identity by default
        math::Mat4 view = cam.get_view_matrix();
        CHECK(view.at(3, 3) == 1.0f);
        
        // At (0,0,3) looking at (0,0,0), view matrix should have -3 translation in Z
        // Actually, look_at(eye, target, up) -> eye is at 0,0,3. 
        // View matrix transforms world to view space.
        math::Vec4 world_pos(0, 0, 3, 1);
        math::Vec4 view_pos = view * world_pos;
        CHECK_THAT(view_pos.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }
}
