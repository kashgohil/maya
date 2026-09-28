#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "maya/core/engine.hpp"
#include "maya/rhi/null_device.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Session {
    std::vector<std::string> events;
    bool device_success = true;
    bool device_throws = false;
    bool application_success = true;
    bool application_throws = false;
    bool render_throws = false;
    bool resize_throws = false;
    bool allow_close = true;
    bool close_throws = false;
    std::vector<maya::FrameTiming> timings;
    bool alive = false;
    bool input_enabled = true;
};

class LifecycleDevice final : public maya::NullGraphicsDevice {
public:
    explicit LifecycleDevice(Session& session) : m_session(session) {}
    ~LifecycleDevice() override {
        shutdown();
        m_session.events.push_back("device.destroy");
    }
protected:
    bool backend_initialize(void* window, maya::RhiLimits& limits, maya::Format& format) override {
        m_session.events.push_back("device.start");
        m_session.alive = true; // Represents a resource acquired before a possible failure.
        if (m_session.device_throws) throw std::runtime_error("device failure");
        return m_session.device_success && NullGraphicsDevice::backend_initialize(window, limits, format);
    }
    void backend_shutdown() noexcept override {
        m_session.events.push_back("device.stop");
        m_session.alive = false;
        NullGraphicsDevice::backend_shutdown();
    }
    void backend_resize(uint32_t, uint32_t) override { m_session.events.push_back("device.resize"); }
    maya::RhiDiagnostic backend_begin_frame() override {
        m_session.events.push_back("frame.begin");
        return {};
    }
    void backend_submit(uint64_t serial, bool present) override {
        m_session.events.push_back("frame.end");
        NullGraphicsDevice::backend_submit(serial, present);
    }
private:
    Session& m_session;
};

class LifecycleApplication final : public maya::Application {
public:
    explicit LifecycleApplication(Session& session) : m_session(session) {}
    ~LifecycleApplication() override {
        m_session.events.push_back(m_session.alive ? "app.destroy.live" : "app.destroy.dead");
    }
    bool on_start(maya::GraphicsDevice&) override {
        m_session.events.push_back("app.start");
        if (m_session.application_throws) throw std::runtime_error("application failure");
        return m_session.application_success;
    }
    void on_update(float, bool input_enabled) override {
        m_session.events.push_back("app.update");
        m_session.input_enabled = input_enabled;
    }
    void on_render(maya::GraphicsDevice&) override {
        m_session.events.push_back("app.render");
        if (m_session.render_throws) throw std::runtime_error("render failure");
    }
    void on_resize(uint32_t, uint32_t) override {
        m_session.events.push_back("app.resize");
        if (m_session.resize_throws) throw std::runtime_error("resize failure");
    }
    void on_frame_timing(const maya::FrameTiming& timing) override { m_session.timings.push_back(timing); }
    bool on_close_requested() override {
        m_session.events.push_back("app.close");
        if (m_session.close_throws) throw std::runtime_error("close failure");
        return m_session.allow_close;
    }
    void on_stop() noexcept override {
        m_session.events.push_back(m_session.alive ? "app.stop.live" : "app.stop.dead");
    }
private:
    Session& m_session;
};

bool start(maya::Engine& engine, Session& session) {
    return engine.initialize(std::make_unique<LifecycleDevice>(session), nullptr,
        std::make_unique<LifecycleApplication>(session));
}

} // namespace

TEST_CASE("Engine owns a session and releases content before its device", "[core][engine]") {
    Session session;
    {
        maya::Engine engine;
        REQUIRE(start(engine, session));
        REQUIRE(engine.is_initialized());
        REQUIRE(engine.resize(1920, 1080));
        REQUIRE(engine.tick(1.0f / 60.0f, false));
        CHECK_FALSE(session.input_enabled);
    }
    CHECK(session.events == std::vector<std::string>{
        "device.start", "app.start", "device.resize", "app.resize",
        "app.update", "frame.begin", "app.render", "frame.end",
        "app.stop.live", "app.destroy.live", "device.stop", "device.destroy"});
}

TEST_CASE("Engine rolls back device failure without starting content", "[core][engine]") {
    Session session;
    SECTION("reported failure") { session.device_success = false; }
    SECTION("exception after allocation") { session.device_throws = true; }
    maya::Engine engine;
    CHECK_FALSE(start(engine, session));
    CHECK_FALSE(engine.is_initialized());
    CHECK_FALSE(engine.tick(0.1f));
    engine.shutdown();
    // A failed device initialization rolls itself back before the unstarted content is destroyed.
    CHECK(session.events == std::vector<std::string>{
        "device.start", "device.stop", "app.destroy.dead", "device.destroy"});
}

TEST_CASE("Engine stops partially started content exactly once", "[core][engine]") {
    Session session;
    SECTION("reported failure") { session.application_success = false; }
    SECTION("exception after allocation") { session.application_throws = true; }
    maya::Engine engine;
    CHECK_FALSE(start(engine, session));
    engine.shutdown();
    CHECK(session.events == std::vector<std::string>{
        "device.start", "app.start", "app.stop.live", "app.destroy.live",
        "device.stop", "device.destroy"});
}

TEST_CASE("Engine supports repeated sessions after failure and shutdown", "[core][engine]") {
    Session failed;
    failed.application_success = false;
    maya::Engine engine;
    CHECK_FALSE(start(engine, failed));
    for (int i = 0; i < 3; ++i) {
        Session session;
        REQUIRE(start(engine, session));
        REQUIRE(engine.tick(0.01f));
        engine.shutdown();
        const auto events = session.events;
        engine.shutdown();
        CHECK(session.events == events);
        CHECK_FALSE(session.alive);
        CHECK_FALSE(engine.is_initialized());
    }
}

TEST_CASE("Engine rejects a second initialization without replacing the active session", "[core][engine]") {
    Session first;
    Session second;
    maya::Engine engine;
    REQUIRE(start(engine, first));
    CHECK_FALSE(start(engine, second));
    CHECK(first.alive);
    REQUIRE(engine.tick(0.01f));
    CHECK(second.events.size() == 2); // Unstarted arguments are simply destroyed.
}

TEST_CASE("Engine rolls back frame and resize exceptions", "[core][engine]") {
    Session session;
    maya::Engine engine;
    REQUIRE(start(engine, session));
    SECTION("render exception") {
        session.render_throws = true;
        CHECK_FALSE(engine.tick(0.01f));
    }
    SECTION("resize exception") {
        session.resize_throws = true;
        CHECK_FALSE(engine.resize(640, 480));
    }
    CHECK_FALSE(engine.is_initialized());
    CHECK_FALSE(session.alive);
    REQUIRE(session.events.size() >= 4);
    CHECK(std::vector<std::string>(session.events.end() - 4, session.events.end()) ==
        std::vector<std::string>{"app.stop.live", "app.destroy.live", "device.stop", "device.destroy"});
}

TEST_CASE("Engine handles idle calls and invalid frame input", "[core][engine]") {
    Session session;
    maya::Engine engine;
    engine.shutdown();
    CHECK_FALSE(engine.tick(0.01f));
    CHECK_FALSE(engine.resize(100, 100));
    CHECK_FALSE(engine.initialize(nullptr, nullptr, nullptr));
    REQUIRE(start(engine, session));
    const auto events = session.events;
    CHECK_FALSE(engine.tick(-1.0f));
    CHECK_FALSE(engine.tick(std::numeric_limits<float>::infinity()));
    CHECK_FALSE(engine.tick(std::numeric_limits<float>::quiet_NaN()));
    CHECK(engine.resize(0, 0));
    CHECK(session.events == events);
}

TEST_CASE("Engine asks the application before the window closes", "[core][engine]") {
    Session session;
    maya::Engine engine;
    CHECK(engine.request_close()); // nothing running, nothing to ask
    CHECK(session.events.empty());
    REQUIRE(start(engine, session));
    session.allow_close = false; // e.g. unsaved changes to ask about
    CHECK_FALSE(engine.request_close());
    CHECK(engine.is_initialized());
    session.allow_close = true;
    CHECK(engine.request_close());
    // An application that fails to answer does not keep the window open.
    session.allow_close = false;
    session.close_throws = true;
    CHECK(engine.request_close());
    CHECK(std::ranges::count(session.events, std::string("app.close")) == 3);
    engine.shutdown();
    CHECK(engine.request_close());
    CHECK(std::ranges::count(session.events, std::string("app.close")) == 3);
}

TEST_CASE("Engine reports where each frame's CPU time went", "[core][engine]") {
    Session session;
    maya::Engine engine;
    REQUIRE(start(engine, session));
    for (int i = 0; i < 3; ++i) REQUIRE(engine.tick(1.0f / 60.0f));
    REQUIRE(session.timings.size() == 3);
    for (size_t i = 0; i < session.timings.size(); ++i) {
        const auto& t = session.timings[i];
        CHECK(t.frame == i + 1); // the frame's submission serial
        for (const auto part : {t.update, t.wait, t.render, t.submit}) CHECK(part >= 0.0);
        CHECK(t.update + t.wait + t.render + t.submit == Catch::Approx(t.tick).margin(1e-9));
    }
    CHECK(session.timings[0].interval == 0.0); // no previous frame
    CHECK(session.timings[1].interval > 0.0);
    CHECK(engine.last_frame_timing().frame == 3);
    // A failed frame reports nothing; a new session starts without an interval.
    session.render_throws = true;
    CHECK_FALSE(engine.tick(1.0f / 60.0f));
    CHECK(session.timings.size() == 3);
    session.render_throws = false;
    REQUIRE(start(engine, session));
    REQUIRE(engine.tick(1.0f / 60.0f));
    CHECK(session.timings.back().interval == 0.0);
    CHECK(session.timings.back().frame == 1);
}
