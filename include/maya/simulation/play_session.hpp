#pragma once

#include "maya/scene/scene_io.hpp"
#include "maya/simulation/simulation.hpp"
#include <memory>
#include <optional>
#include <string>

namespace maya {

class PlaySession;
struct PlayStartResult {
    std::unique_ptr<PlaySession> session; // null on failure
    SceneDiagnostics diagnostics; // why the scene could not be built
    std::string error; // why a system could not start
    explicit operator bool() const noexcept { return static_cast<bool>(session); }
};

/// What one host frame did.
struct PlayFrame {
    ClockAdvance clock;
    uint32_t ticks_run = 0;
    std::string error; // a system failed; the session has stopped simulating
    std::vector<SimulationMessage> messages; // problems systems reported without stopping, in order
};

/// A running scene: its own World built from a scene document, a fixed clock, gameplay input, and
/// an ordered list of systems. The player runs one for its whole life; the editor makes one for each
/// Play and drops it on Stop, so the authored scene is never touched. Single owner thread.
class PlaySession {
public:
    /// Validates the document and builds a new World from it (as a scene file is loaded), then
    /// starts the systems in order. On failure everything started is stopped again.
    static PlayStartResult start(SceneDocument document, const PropertyValidationContext& context,
                                 std::vector<std::unique_ptr<SimulationSystem>> systems, ClockSettings clock = {},
                                 PhysicsSettings physics = {});
    ~PlaySession(); // stops the systems in reverse order, then releases the World
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    /// One host frame: admits `wall_delta` seconds and runs the ticks that are due. Each tick latches
    /// input, runs every system, steps physics once, and commits the systems' commands together with
    /// the moved body poses, then adds and removes bodies. If a system throws, breaks the physics
    /// authority rules, or its commands are rejected, the session stops simulating and reports the
    /// error (again in error()); the World keeps the last tick that committed.
    PlayFrame update(double wall_delta);

    World& world() noexcept { return *m_world; }
    const World& world() const noexcept { return *m_world; }
    /// The session's physics, created with it and destroyed before its World.
    const PhysicsWorld& physics() const noexcept { return *m_physics; }
    FixedClock& clock() noexcept { return m_clock; }
    const FixedClock& clock() const noexcept { return m_clock; }
    GameInput& input() noexcept { return m_input; }
    /// The first camera in document order (roots in order, each followed by its descendants), when
    /// the scene has one. It is the view the player shows.
    std::optional<EntityId> camera() const noexcept { return m_camera; }
    bool failed() const noexcept { return !m_error.empty(); }
    const std::string& error() const noexcept { return m_error; }

private:
    PlaySession(std::unique_ptr<World> world, std::vector<std::unique_ptr<SimulationSystem>> systems,
                ClockSettings clock, PhysicsSettings physics, std::optional<EntityId> camera);
    void run_tick(std::vector<SimulationMessage>& messages);

    std::unique_ptr<World> m_world;
    std::unique_ptr<PhysicsWorld> m_physics; // declared after the World, so it is destroyed first
    std::vector<std::unique_ptr<SimulationSystem>> m_systems;
    size_t m_started = 0; // systems whose start was entered
    FixedClock m_clock;
    GameInput m_input;
    std::optional<EntityId> m_camera;
    std::string m_error;
};

} // namespace maya
