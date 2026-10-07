#pragma once

#include "maya/physics/physics.hpp"
#include "maya/platform/input.hpp"
#include "maya/world/world.hpp"
#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace maya {

// Fixed-step play simulation shared by the player and editor play sessions. The rules follow
// docs/architecture/scheduling-contracts.md; see docs/play.md.

struct ClockSettings {
    uint32_t ticks_per_second = 60;
    uint32_t max_catch_up_ticks = 4; // ticks run in one host frame at most
    double max_frame_delta = 0.25; // wall seconds admitted in one host frame at most
};

/// What one host frame admitted.
struct ClockAdvance {
    uint32_t ticks = 0; // fixed ticks to run now, in order
    double rejected_time = 0.0; // wall time beyond max_frame_delta, not simulated
    uint32_t discarded_ticks = 0; // whole ticks beyond max_catch_up_ticks, not simulated
    bool invalid_delta = false; // a negative or nonfinite delta, ignored
};

/// Integer ticks of a fixed interval, fed by wall time. Time accumulates in double precision;
/// simulation time is derived from the tick count. Paused wall time does not accumulate, and resuming
/// starts from an empty accumulator, so there is no catch-up burst.
class FixedClock {
public:
    /// Throws std::invalid_argument for zero rates or caps, or a nonpositive or nonfinite delta cap.
    explicit FixedClock(ClockSettings settings = {});

    /// Admits a host frame's wall time and returns how many ticks are due; the fractional remainder
    /// is kept for the next frame. The runner calls count_tick() as each of them completes.
    ClockAdvance advance(double wall_delta);
    void count_tick() noexcept { ++m_tick; }
    void pause() noexcept { m_paused = true; }
    void resume() noexcept;
    bool paused() const noexcept { return m_paused; }
    /// While paused, the next advance runs exactly one tick. Does nothing while running.
    void step() noexcept;

    const ClockSettings& settings() const noexcept { return m_settings; }
    double interval() const noexcept { return 1.0 / m_settings.ticks_per_second; }
    uint64_t tick() const noexcept { return m_tick; } // ticks completed
    double time() const noexcept { return double(m_tick) * interval(); }
    /// Progress towards the next tick, in [0, 1); 1 while paused, where the current pose is shown.
    float alpha() const noexcept;
    double total_rejected_time() const noexcept { return m_rejected; }
    uint64_t total_discarded_ticks() const noexcept { return m_discarded; }

private:
    ClockSettings m_settings;
    double m_accumulator = 0.0;
    uint64_t m_tick = 0;
    bool m_paused = false;
    bool m_step = false;
    double m_rejected = 0.0;
    uint64_t m_discarded = 0;
};

/// Gameplay input seen by one tick.
struct InputFrame {
    static constexpr size_t key_count = 512; // covers every GLFW key code
    std::bitset<key_count> held, pressed, released;
    std::bitset<4> buttons_held, buttons_pressed, buttons_released; // MouseButton order
    math::Vec2 look{0.0f, 0.0f}; // pointer movement in points since the previous tick
    float scroll = 0.0f;

    bool down(KeyCode key) const noexcept;
    bool went_down(KeyCode key) const noexcept;
    bool went_up(KeyCode key) const noexcept;
};

/// Turns window events owned by the game into per-tick input. Held state persists across ticks.
/// A press or release edge, pointer movement, and scrolling go to the next tick that runs, in the
/// order they happened (a press and release before that tick report both, with the key not held).
/// Catch-up ticks in the same frame see held state only, and a frame without ticks keeps its edges
/// for a later one. Losing focus, or handing input back to the UI, releases everything.
class GameInput {
public:
    void feed(const std::vector<InputEvent>& events);
    /// The input for the next tick to run; it takes the pending edges and movement.
    InputFrame latch();
    void release_all();
    const InputFrame& pending() const noexcept { return m_pending; }

private:
    InputFrame m_pending;
    std::optional<math::Vec2> m_pointer;
};

/// Something a system reports without stopping the session: a failed script instance (an error), a
/// value it could not use (a warning), or a script's own log line (info).
struct SimulationMessage {
    enum class Level { info, warning, error };
    Level level = Level::error;
    std::string source; // the system's name
    std::string text;
    uint64_t tick = 0;
};

/// What a system sees during one fixed tick: in fixed_update (phase 3), before the step, and in
/// late_fixed_update (phase 7), after it.
struct TickContext {
    const World& world; // the state committed by the previous tick
    WorldCommands& commands; // committed at the end of the tick (the next tick's phase 1), in system order
    const InputFrame& input;
    uint64_t tick; // the index of this tick, from 0
    double time; // simulation seconds at the start of this tick
    float delta; // the fixed interval, in seconds
    /// Physics requests. In fixed_update they apply before this tick's step; in late_fixed_update,
    /// before the next tick's. Created and removed bodies take effect when this tick commits.
    BodyCommands& bodies;
    const PhysicsWorld& physics; // bodies after the last completed step: the previous tick's, or in phase 7 this tick's
    std::vector<SimulationMessage>& messages; // reported with the host frame (PlayFrame::messages)
    /// late_fixed_update only: this step's contact and trigger events, sorted (PhysicsEvent).
    std::span<const PhysicsEvent> events = {};
    /// late_fixed_update only: the session is stopping, the events end every contact still in
    /// progress, and nothing the systems do is kept.
    bool stopping = false;
    /// Entities whose pose jumps in this tick, as a teleport's does: they and their descendants are shown
    /// at their new pose rather than between the two (PlaySession::presentation). None when null.
    std::vector<EntityHandle>* jumps = nullptr;
};

/// What a system sees once per host frame, after the frame's ticks: read-only.
struct FrameContext {
    const World& world;
    const PhysicsWorld& physics;
    double frame_delta; // wall seconds admitted for this frame
    float alpha; // progress towards the next tick
    uint64_t tick; // ticks completed
    double time; // simulation seconds
    std::vector<SimulationMessage>& messages;
};

/// One step of play simulation, run in a fixed order with the others. Systems keep their own state;
/// they read the World and write through the tick's commands, so one system's writes are visible to
/// the next tick, not to later systems in the same tick. Each transform should have one writer: a
/// system that sets the transform of a kinematic or dynamic body fails, since physics writes it.
class SimulationSystem {
public:
    virtual ~SimulationSystem() = default;
    virtual std::string_view name() const = 0;
    /// Once, before the first tick. A failure (an exception) stops the session from starting.
    virtual void start(const World&) {}
    virtual void fixed_update(TickContext& tick) = 0;
    /// Phase 7, after the step: `tick.events` holds its contact and trigger events and `tick.physics`
    /// its results. Changes go into this tick's batch; body requests apply before the next step.
    /// Also once when the session stops, with the events that end every contact (`tick.stopping`).
    virtual void late_fixed_update(TickContext&) {}
    /// Once per host frame after its ticks, also while paused. Read-only: effects wait for a tick.
    virtual void frame(FrameContext&) {}
    /// Once, if start was entered, including when it threw.
    virtual void stop() noexcept {}
};

/// The built-in behaviors, in order: fly control (maya.fly_control), then spin (maya.spin).
std::vector<std::unique_ptr<SimulationSystem>> builtin_systems();

} // namespace maya
