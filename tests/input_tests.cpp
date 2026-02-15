#include <catch2/catch_test_macros.hpp>
#include "maya/platform/input.hpp"

using namespace maya;
using namespace maya::math;

// =============================================================================
// Input Key State Tests
// =============================================================================
TEST_CASE("Input key down state", "[platform][input]") {
    Input& input = Input::instance();
    
    // Reset state
    input.update();
    
    SECTION("Key not pressed returns false") {
        CHECK(input.is_key_down(KeyCode::A) == false);
        CHECK(input.is_key_down(KeyCode::W) == false);
        CHECK(input.is_key_down(KeyCode::Space) == false);
    }

    SECTION("Key pressed returns true") {
        input.set_key_state(KeyCode::A, true);
        CHECK(input.is_key_down(KeyCode::A) == true);
    }

    SECTION("Key released returns false") {
        input.set_key_state(KeyCode::A, true);
        CHECK(input.is_key_down(KeyCode::A) == true);
        
        input.set_key_state(KeyCode::A, false);
        CHECK(input.is_key_down(KeyCode::A) == false);
    }

    SECTION("Multiple keys tracked independently") {
        input.set_key_state(KeyCode::A, true);
        input.set_key_state(KeyCode::D, true);
        input.set_key_state(KeyCode::W, false);
        
        CHECK(input.is_key_down(KeyCode::A) == true);
        CHECK(input.is_key_down(KeyCode::D) == true);
        CHECK(input.is_key_down(KeyCode::W) == false);
    }
}

TEST_CASE("Input key pressed detection", "[platform][input]") {
    Input& input = Input::instance();
    
    // Clear all key states first
    input.update();
    input.set_key_state(KeyCode::A, false);
    input.set_key_state(KeyCode::D, false);
    input.set_key_state(KeyCode::W, false);
    input.update();

    SECTION("Key pressed returns true on first frame") {
        input.set_key_state(KeyCode::A, true);
        CHECK(input.is_key_pressed(KeyCode::A) == true);
    }

    SECTION("Key pressed returns false on subsequent frames") {
        input.set_key_state(KeyCode::A, true);
        CHECK(input.is_key_pressed(KeyCode::A) == true);  // First check
        
        input.update();  // Advance frame
        CHECK(input.is_key_pressed(KeyCode::A) == false); // Second check
        CHECK(input.is_key_down(KeyCode::A) == true);      // Still down
    }

    SECTION("Key released then pressed again") {
        // Ensure key starts released
        input.set_key_state(KeyCode::A, false);
        input.update();
        
        // Press
        input.set_key_state(KeyCode::A, true);
        CHECK(input.is_key_pressed(KeyCode::A) == true);
        input.update();
        
        // Release
        input.set_key_state(KeyCode::A, false);
        CHECK(input.is_key_pressed(KeyCode::A) == false);
        input.update();
        
        // Press again
        input.set_key_state(KeyCode::A, true);
        CHECK(input.is_key_pressed(KeyCode::A) == true);
    }

    SECTION("Multiple key presses in same frame") {
        input.set_key_state(KeyCode::A, true);
        input.set_key_state(KeyCode::D, true);
        input.set_key_state(KeyCode::W, true);
        
        CHECK(input.is_key_pressed(KeyCode::A) == true);
        CHECK(input.is_key_pressed(KeyCode::D) == true);
        CHECK(input.is_key_pressed(KeyCode::W) == true);
    }

    SECTION("Rapid key presses between updates") {
        // Simulate rapid press-release-press between frames
        input.set_key_state(KeyCode::A, true);
        input.set_key_state(KeyCode::A, false);
        input.set_key_state(KeyCode::A, true);
        
        // Should see pressed (current state is down, previous was down from earlier)
        // Actually depends on previous state, let's be more explicit
        input.update();
        input.set_key_state(KeyCode::A, false);
        input.update();
        input.set_key_state(KeyCode::A, true);
        
        CHECK(input.is_key_pressed(KeyCode::A) == true);
    }
}

TEST_CASE("Input update preserves state", "[platform][input]") {
    Input& input = Input::instance();
    
    // Clear and set initial state
    input.update();
    input.set_key_state(KeyCode::A, true);
    input.set_key_state(KeyCode::W, true);
    
    input.update();
    
    SECTION("Keys remain down after update") {
        CHECK(input.is_key_down(KeyCode::A) == true);
        CHECK(input.is_key_down(KeyCode::W) == true);
    }

    SECTION("Pressed state changes after update") {
        // After update, A was down before and is down now
        CHECK(input.is_key_pressed(KeyCode::A) == false);
    }
}

// =============================================================================
// Input Mouse Tests
// =============================================================================
TEST_CASE("Input mouse position", "[platform][input]") {
    Input& input = Input::instance();
    
    SECTION("Default mouse position is zero") {
        const Vec2& pos = input.get_mouse_position();
        CHECK(pos.x == 0.0f);
        CHECK(pos.y == 0.0f);
    }

    SECTION("Mouse position can be set") {
        input.set_mouse_position(100.0f, 200.0f);
        const Vec2& pos = input.get_mouse_position();
        CHECK(pos.x == 100.0f);
        CHECK(pos.y == 200.0f);
    }

    SECTION("Mouse position updated") {
        input.set_mouse_position(50.0f, 75.0f);
        const Vec2& pos1 = input.get_mouse_position();
        CHECK(pos1.x == 50.0f);
        CHECK(pos1.y == 75.0f);
        
        input.set_mouse_position(150.0f, 250.0f);
        const Vec2& pos2 = input.get_mouse_position();
        CHECK(pos2.x == 150.0f);
        CHECK(pos2.y == 250.0f);
    }

    SECTION("Mouse position survives update") {
        input.set_mouse_position(300.0f, 400.0f);
        input.update();
        
        const Vec2& pos = input.get_mouse_position();
        CHECK(pos.x == 300.0f);
        CHECK(pos.y == 400.0f);
    }
}

// =============================================================================
// Input Edge Cases
// =============================================================================
TEST_CASE("Input edge cases", "[platform][input]") {
    Input& input = Input::instance();
    
    // Reset
    input.update();
    input.update();

    SECTION("Unknown key code") {
        // KeyCode::Unknown should work like any other key
        input.set_key_state(KeyCode::Unknown, true);
        CHECK(input.is_key_down(KeyCode::Unknown) == true);
        CHECK(input.is_key_pressed(KeyCode::Unknown) == true);
    }

    SECTION("All standard keys") {
        // Test various key codes
        input.set_key_state(KeyCode::Space, true);
        input.set_key_state(KeyCode::Escape, true);
        input.set_key_state(KeyCode::Enter, true);
        input.set_key_state(KeyCode::Up, true);
        input.set_key_state(KeyCode::Down, true);
        input.set_key_state(KeyCode::Left, true);
        input.set_key_state(KeyCode::Right, true);
        
        CHECK(input.is_key_down(KeyCode::Space) == true);
        CHECK(input.is_key_down(KeyCode::Escape) == true);
        CHECK(input.is_key_down(KeyCode::Enter) == true);
        CHECK(input.is_key_down(KeyCode::Up) == true);
        CHECK(input.is_key_down(KeyCode::Down) == true);
        CHECK(input.is_key_down(KeyCode::Left) == true);
        CHECK(input.is_key_down(KeyCode::Right) == true);
    }

    SECTION("Negative mouse coordinates") {
        input.set_mouse_position(-100.0f, -50.0f);
        const Vec2& pos = input.get_mouse_position();
        CHECK(pos.x == -100.0f);
        CHECK(pos.y == -50.0f);
    }

    SECTION("Large mouse coordinates") {
        input.set_mouse_position(10000.0f, 5000.0f);
        const Vec2& pos = input.get_mouse_position();
        CHECK(pos.x == 10000.0f);
        CHECK(pos.y == 5000.0f);
    }

    SECTION("Zero mouse coordinates") {
        input.set_mouse_position(0.0f, 0.0f);
        const Vec2& pos = input.get_mouse_position();
        CHECK(pos.x == 0.0f);
        CHECK(pos.y == 0.0f);
    }

    SECTION("Fractional mouse coordinates") {
        input.set_mouse_position(100.5f, 200.75f);
        const Vec2& pos = input.get_mouse_position();
        CHECK(pos.x == 100.5f);
        CHECK(pos.y == 200.75f);
    }
}

// =============================================================================
// Input State Transition Tests
// =============================================================================
TEST_CASE("Input state transitions", "[platform][input]") {
    Input& input = Input::instance();
    
    // Start fresh
    input.update();
    input.update();

    SECTION("Complete key lifecycle: up -> pressed -> down -> released") {
        // Frame 1: Key up
        input.set_key_state(KeyCode::A, false);
        input.update();
        
        CHECK(input.is_key_down(KeyCode::A) == false);
        CHECK(input.is_key_pressed(KeyCode::A) == false);
        
        // Frame 2: Key pressed
        input.set_key_state(KeyCode::A, true);
        CHECK(input.is_key_down(KeyCode::A) == true);
        CHECK(input.is_key_pressed(KeyCode::A) == true);
        input.update();
        
        // Frame 3: Key held down
        CHECK(input.is_key_down(KeyCode::A) == true);
        CHECK(input.is_key_pressed(KeyCode::A) == false);
        
        // Frame 4: Key released
        input.set_key_state(KeyCode::A, false);
        CHECK(input.is_key_down(KeyCode::A) == false);
        CHECK(input.is_key_pressed(KeyCode::A) == false);
    }

    SECTION("Multiple update cycles") {
        // Ensure key starts released
        input.set_key_state(KeyCode::W, false);
        input.update();
        
        input.set_key_state(KeyCode::W, true);
        
        // First check - transition from false to true
        CHECK(input.is_key_pressed(KeyCode::W) == true);
        input.update();
        
        // Multiple updates - key remains down
        for (int i = 0; i < 5; ++i) {
            CHECK(input.is_key_down(KeyCode::W) == true);
            CHECK(input.is_key_pressed(KeyCode::W) == false);
            input.update();
        }
        
        // Release
        input.set_key_state(KeyCode::W, false);
        CHECK(input.is_key_down(KeyCode::W) == false);
    }
}
