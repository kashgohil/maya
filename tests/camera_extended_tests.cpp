#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "maya/core/camera.hpp"
#include "maya/math/math_utils.hpp"

using namespace maya;
using namespace maya::math;

// =============================================================================
// Camera Projection Matrix Tests
// =============================================================================
TEST_CASE("Camera projection matrix", "[core][camera]") {
    Camera cam(60.0f, 16.0f/9.0f, 0.1f, 100.0f);

    SECTION("Projection matrix is created") {
        Mat4 proj = cam.get_projection_matrix();
        // Just verify it doesn't crash and has valid values
        CHECK(proj.at(0, 0) != 0.0f); // Should have fov scaling
        CHECK(proj.at(1, 1) != 0.0f); // Should have aspect scaling
        CHECK(proj.at(2, 2) != 0.0f); // Should have depth scaling
    }

    SECTION("Projection matrix perspective divide") {
        Mat4 proj = cam.get_projection_matrix();
        
        // Test near plane maps to depth 0
        Vec4 near_point(0.0f, 0.0f, -0.1f, 1.0f);
        Vec4 near_result = proj * near_point;
        float near_depth = near_result.z / near_result.w;
        CHECK_THAT(near_depth, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        
        // Test far plane maps to depth 1
        Vec4 far_point(0.0f, 0.0f, -100.0f, 1.0f);
        Vec4 far_result = proj * far_point;
        float far_depth = far_result.z / far_result.w;
        CHECK_THAT(far_depth, Catch::Matchers::WithinAbs(1.0f, 0.0001f));
    }

    SECTION("Different FOV values") {
        Camera narrow(30.0f, 16.0f/9.0f, 0.1f, 100.0f);
        Camera wide(90.0f, 16.0f/9.0f, 0.1f, 100.0f);
        
        Mat4 narrow_proj = narrow.get_projection_matrix();
        Mat4 wide_proj = wide.get_projection_matrix();
        
        // Narrow FOV should have larger scaling factor
        CHECK(narrow_proj.at(0, 0) > wide_proj.at(0, 0));
    }

    SECTION("Different aspect ratios") {
        Camera widescreen(60.0f, 16.0f/9.0f, 0.1f, 100.0f);
        Camera square(60.0f, 1.0f, 0.1f, 100.0f);
        
        Mat4 wide_proj = widescreen.get_projection_matrix();
        Mat4 square_proj = square.get_projection_matrix();
        
        // Widescreen should have different X scaling
        CHECK(wide_proj.at(0, 0) != square_proj.at(0, 0));
    }

    SECTION("Different clip planes") {
        Camera close_clip(60.0f, 16.0f/9.0f, 0.1f, 10.0f);
        Camera far_clip(60.0f, 16.0f/9.0f, 10.0f, 1000.0f);
        
        Mat4 close_proj = close_clip.get_projection_matrix();
        Mat4 far_proj = far_clip.get_projection_matrix();
        
        // Different clip planes should produce different matrices
        // Check m[2][3] which is (far * near) / (near - far)
        CHECK(close_proj.at(2, 3) != far_proj.at(2, 3));
    }
}

// =============================================================================
// Camera View Matrix Tests
// =============================================================================
TEST_CASE("Camera view matrix", "[core][camera]") {
    Camera cam(60.0f, 16.0f/9.0f, 0.1f, 100.0f);

    SECTION("View matrix transforms eye to origin") {
        Vec3 pos(5.0f, 3.0f, 10.0f);
        cam.set_position(pos);
        
        Mat4 view = cam.get_view_matrix();
        Vec4 eye_world(pos.x, pos.y, pos.z, 1.0f);
        Vec4 eye_view = view * eye_world;
        
        CHECK_THAT(eye_view.x, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(eye_view.y, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
        CHECK_THAT(eye_view.z, Catch::Matchers::WithinAbs(0.0f, 0.0001f));
    }

    SECTION("View matrix from different positions") {
        Vec3 positions[] = {
            Vec3(0.0f, 0.0f, 5.0f),
            Vec3(10.0f, 5.0f, 0.0f),
            Vec3(-5.0f, -3.0f, -8.0f)
        };
        
        for (const auto& pos : positions) {
            cam.set_position(pos);
            Mat4 view = cam.get_view_matrix();
            
            Vec4 eye_world(pos.x, pos.y, pos.z, 1.0f);
            Vec4 eye_view = view * eye_world;
            
            CHECK_THAT(eye_view.x, Catch::Matchers::WithinAbs(0.0f, 0.01f));
            CHECK_THAT(eye_view.y, Catch::Matchers::WithinAbs(0.0f, 0.01f));
            CHECK_THAT(eye_view.z, Catch::Matchers::WithinAbs(0.0f, 0.01f));
        }
    }
}

// =============================================================================
// Camera View-Projection Matrix Tests
// =============================================================================
TEST_CASE("Camera view-projection matrix", "[core][camera]") {
    Camera cam(60.0f, 16.0f/9.0f, 0.1f, 100.0f);
    cam.set_position(Vec3(0.0f, 0.0f, 3.0f));

    SECTION("VP matrix is projection * view") {
        Mat4 view = cam.get_view_matrix();
        Mat4 proj = cam.get_projection_matrix();
        Mat4 vp = cam.get_view_projection_matrix();
        
        Mat4 manual_vp = proj * view;
        
        // Verify they match
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                CHECK_THAT(vp.at(i, j), Catch::Matchers::WithinAbs(manual_vp.at(i, j), 0.0001f));
            }
        }
    }

    SECTION("VP transforms world to clip space") {
        Mat4 vp = cam.get_view_projection_matrix();
        
        // Point in front of camera should be within clip space
        Vec4 world_point(0.0f, 0.0f, 2.0f, 1.0f); // 1 unit in front of camera at (0,0,3)
        Vec4 clip_point = vp * world_point;
        
        // After perspective divide, should be near center of screen
        float x_ndc = clip_point.x / clip_point.w;
        float y_ndc = clip_point.y / clip_point.w;
        
        CHECK_THAT(x_ndc, Catch::Matchers::WithinAbs(0.0f, 0.1f));
        CHECK_THAT(y_ndc, Catch::Matchers::WithinAbs(0.0f, 0.1f));
    }
}

// =============================================================================
// Camera Position Tests
// =============================================================================
TEST_CASE("Camera position", "[core][camera]") {
    Camera cam(60.0f, 16.0f/9.0f, 0.1f, 100.0f);

    SECTION("Default position") {
        Vec3 pos = cam.get_position();
        // Default is (0, 0, 3) from constructor
        CHECK(pos.x == 0.0f);
        CHECK(pos.y == 0.0f);
        CHECK(pos.z == 3.0f);
    }

    SECTION("Set position") {
        cam.set_position(Vec3(10.0f, 5.0f, -2.0f));
        Vec3 pos = cam.get_position();
        
        CHECK(pos.x == 10.0f);
        CHECK(pos.y == 5.0f);
        CHECK(pos.z == -2.0f);
    }

    SECTION("Position affects view matrix") {
        cam.set_position(Vec3(5.0f, 0.0f, 0.0f));
        Mat4 view1 = cam.get_view_matrix();
        
        cam.set_position(Vec3(0.0f, 5.0f, 0.0f));
        Mat4 view2 = cam.get_view_matrix();
        
        // Different positions should produce different view matrices
        bool same = true;
        for (int i = 0; i < 4 && same; ++i) {
            for (int j = 0; j < 4 && same; ++j) {
                if (std::abs(view1.at(i, j) - view2.at(i, j)) > 0.0001f) {
                    same = false;
                }
            }
        }
        CHECK(!same);
    }
}

// =============================================================================
// Camera Pitch Constraint Tests
// =============================================================================
TEST_CASE("Camera pitch constraints", "[core][camera]") {
    // Note: Pitch constraints are applied during mouse processing
    // which requires Input singleton. We can verify the camera
    // starts with valid pitch values.
    
    Camera cam(60.0f, 16.0f/9.0f, 0.1f, 100.0f);

    SECTION("Camera initializes with valid pitch") {
        // Default pitch should be 0 (from constructor)
        // We can't directly test pitch constraints without mocking Input
        // but we can verify the camera works correctly
        CHECK_NOTHROW(cam.update(0.016f));
    }

    SECTION("Camera with extreme FOV") {
        Camera narrow(1.0f, 16.0f/9.0f, 0.1f, 100.0f);
        Camera wide(120.0f, 16.0f/9.0f, 0.1f, 100.0f);
        
        // Both should create valid projection matrices
        Mat4 narrow_proj = narrow.get_projection_matrix();
        Mat4 wide_proj = wide.get_projection_matrix();
        
        CHECK(narrow_proj.at(0, 0) > 0.0f);
        CHECK(wide_proj.at(0, 0) > 0.0f);
    }
}

// =============================================================================
// Camera Edge Cases
// =============================================================================
TEST_CASE("Camera edge cases", "[core][camera]") {
    SECTION("Camera at origin") {
        Camera cam(60.0f, 16.0f/9.0f, 0.1f, 100.0f);
        cam.set_position(Vec3(0.0f, 0.0f, 0.0f));
        
        Mat4 view = cam.get_view_matrix();
        // Should not crash, though view from origin may be degenerate
        CHECK(true);
    }

    SECTION("Camera with square aspect ratio") {
        Camera cam(60.0f, 1.0f, 0.1f, 100.0f);
        Mat4 proj = cam.get_projection_matrix();
        
        // X and Y scaling should be equal for square aspect
        CHECK_THAT(proj.at(0, 0), Catch::Matchers::WithinRel(proj.at(1, 1), 0.0001f));
    }

    SECTION("Camera with very wide aspect ratio") {
        Camera cam(60.0f, 21.0f/9.0f, 0.1f, 100.0f);
        Mat4 proj = cam.get_projection_matrix();
        
        // Should create valid projection
        CHECK(proj.at(0, 0) != 0.0f);
        CHECK(proj.at(1, 1) != 0.0f);
    }

    SECTION("Camera with very narrow aspect ratio") {
        Camera cam(60.0f, 4.0f/3.0f, 0.1f, 100.0f);
        Mat4 proj = cam.get_projection_matrix();
        
        // Should create valid projection
        CHECK(proj.at(0, 0) != 0.0f);
        CHECK(proj.at(1, 1) != 0.0f);
    }

    SECTION("Camera with near = far") {
        // This would create a degenerate projection
        // Camera constructor doesn't validate this, just test it doesn't crash
        Camera cam(60.0f, 16.0f/9.0f, 1.0f, 1.0f);
        Mat4 proj = cam.get_projection_matrix();
        
        // Should not crash, though projection is degenerate
        CHECK(true);
    }

    SECTION("Camera with zero near plane") {
        Camera cam(60.0f, 16.0f/9.0f, 0.0f, 100.0f);
        Mat4 proj = cam.get_projection_matrix();
        
        // Division by zero in perspective calculation
        // Just verify it doesn't crash
        CHECK(true);
    }
}

// =============================================================================
// Camera Multiple Instance Tests
// =============================================================================
TEST_CASE("Camera multiple instances", "[core][camera]") {
    Camera cam1(60.0f, 16.0f/9.0f, 0.1f, 100.0f);
    Camera cam2(90.0f, 1.0f, 0.01f, 1000.0f);
    
    cam1.set_position(Vec3(0.0f, 0.0f, 5.0f));
    cam2.set_position(Vec3(10.0f, 5.0f, 0.0f));
    
    SECTION("Independent positions") {
        CHECK(cam1.get_position().x == 0.0f);
        CHECK(cam2.get_position().x == 10.0f);
    }

    SECTION("Independent projections") {
        Mat4 proj1 = cam1.get_projection_matrix();
        Mat4 proj2 = cam2.get_projection_matrix();
        
        CHECK(proj1.at(0, 0) != proj2.at(0, 0)); // Different FOV
    }

    SECTION("Independent view matrices") {
        Mat4 view1 = cam1.get_view_matrix();
        Mat4 view2 = cam2.get_view_matrix();
        
        // Should be different due to different positions
        bool same = true;
        for (int i = 0; i < 4 && same; ++i) {
            for (int j = 0; j < 4 && same; ++j) {
                if (std::abs(view1.at(i, j) - view2.at(i, j)) > 0.0001f) {
                    same = false;
                }
            }
        }
        CHECK(!same);
    }
}
