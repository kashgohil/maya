#include <catch2/catch_test_macros.hpp>
#include "maya/core/engine.hpp"
#include "maya/platform/window.hpp"

namespace {
class FailingApplication final : public maya::Application {
public:
    bool on_start(maya::GraphicsDevice& device) override {
        // Acquire a real GPU resource before failing to exercise rollback.
        device.create_uniform_buffer(256);
        return false;
    }
};
}

TEST_CASE("Desktop sessions survive partial startup and repeated shutdown", "[desktop]") {
    maya::Window window(320, 240, "Maya lifecycle test");
    REQUIRE(window.get_native_handle() != nullptr);
    maya::Engine engine;
    CHECK_FALSE(engine.initialize(maya::GraphicsDevice::create_default(),
        window.get_native_handle(), std::make_unique<FailingApplication>()));
    for (int session = 0; session < 3; ++session) {
        REQUIRE(engine.initialize(maya::GraphicsDevice::create_default(),
            window.get_native_handle(), std::make_unique<maya::Application>()));
        REQUIRE(engine.tick(1.0f / 60.0f, false));
        engine.shutdown();
        engine.shutdown();
    }
}

TEST_CASE("Failed and overlapping windows do not invalidate surviving windows", "[desktop]") {
    for (int session = 0; session < 2; ++session) {
        maya::Window first(320, 240, "Maya window lifecycle test");
        REQUIRE(first.get_native_handle() != nullptr);
        {
            maya::Window invalid(0, 0, "Invalid window");
            CHECK(invalid.get_native_handle() == nullptr);
            CHECK(invalid.should_close());
        }
        {
            maya::Window second(320, 240, "Maya second window");
            REQUIRE(second.get_native_handle() != nullptr);
        }
        first.poll_events();
        CHECK(first.get_native_handle() != nullptr);
        CHECK_FALSE(first.should_close());
        CHECK(first.framebuffer_size().first > 0);
    }
}
