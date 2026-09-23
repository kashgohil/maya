#include <catch2/catch_test_macros.hpp>
#include "maya/core/engine.hpp"
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
    bool alive = false;
    bool input_enabled = true;
};

class LifecycleDevice final : public maya::GraphicsDevice {
public:
    explicit LifecycleDevice(Session& session) : m_session(session) {}
    ~LifecycleDevice() override { m_session.events.push_back("device.destroy"); }
    bool initialize(void*) override {
        m_session.events.push_back("device.start");
        m_session.alive = true; // Represents a resource acquired before a possible failure.
        if (m_session.device_throws) throw std::runtime_error("device failure");
        return m_session.device_success;
    }
    void shutdown() override {
        m_session.events.push_back("device.stop");
        m_session.alive = false;
    }
    void resize(uint32_t, uint32_t) override { m_session.events.push_back("device.resize"); }
    void begin_frame() override { m_session.events.push_back("frame.begin"); }
    void end_frame() override { m_session.events.push_back("frame.end"); }
    maya::PipelineHandle create_pipeline(const std::string&, const std::string&, const std::string&) override { return {1}; }
    maya::VertexBufferHandle create_vertex_buffer(const void*, size_t) override { return {1}; }
    maya::IndexBufferHandle create_index_buffer(const void*, size_t) override { return {1}; }
    maya::UniformBufferHandle create_uniform_buffer(size_t) override { return {1}; }
    void update_uniform_buffer(maya::UniformBufferHandle, const void*, size_t) override {}
    maya::TextureHandle create_texture(const void*, uint32_t, uint32_t) override { return {1}; }
    void bind_vertex_buffer(maya::VertexBufferHandle, uint32_t) override {}
    void bind_uniform_buffer(maya::UniformBufferHandle, uint32_t) override {}
    void bind_texture(maya::TextureHandle, uint32_t) override {}
    void draw_indexed(maya::IndexBufferHandle, uint32_t) override {}
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
    CHECK(session.events == std::vector<std::string>{
        "device.start", "app.destroy.live", "device.stop", "device.destroy"});
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
