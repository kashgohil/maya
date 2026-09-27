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
};

/// A running scene: its own World built from a scene document, a fixed clock, gameplay input, and
/// an ordered list of systems. The player runs one for its whole life; the editor makes one for each
/// Play and drops it on Stop, so the authored scene is never touched. Single owner thread.
class PlaySession {
public:
    /// Validates the document and builds a new World from it (as a scene file is loaded), then
    /// starts the systems in order. On failure everything started is stopped again.
    static PlayStartResult start(SceneDocument document, const PropertyValidationContext& context,
                                 std::vector<std::unique_ptr<SimulationSystem>> systems, ClockSettings clock = {});
    ~PlaySession(); // stops the systems in reverse order, then releases the World
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    /// One host frame: admits `wall_delta` seconds and runs the ticks that are due. Each tick latches
    /// input, runs every system, and commits their commands. If a system throws or its commands are
    /// rejected, the session stops simulating, keeps the World as of the last completed tick, and
    /// reports the error (again in error()).
    PlayFrame update(double wall_delta);

    World& world() noexcept { return *m_world; }
    const World& world() const noexcept { return *m_world; }
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
                ClockSettings clock, std::optional<EntityId> camera);
    void run_tick();

    std::unique_ptr<World> m_world;
    std::vector<std::unique_ptr<SimulationSystem>> m_systems;
    size_t m_started = 0; // systems whose start was entered
    FixedClock m_clock;
    GameInput m_input;
    std::optional<EntityId> m_camera;
    std::string m_error;
};

} // namespace maya
