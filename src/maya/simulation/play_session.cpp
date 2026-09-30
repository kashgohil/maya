#include "maya/simulation/play_session.hpp"
#include "maya/simulation/authored_physics.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace maya {

PlaySession::PlaySession(std::unique_ptr<World> world, std::vector<std::unique_ptr<SimulationSystem>> systems,
                         ClockSettings clock, PhysicsSettings physics, std::optional<EntityId> camera)
    : m_world(std::move(world)), m_physics(std::make_unique<PhysicsWorld>(physics)), m_systems(std::move(systems)),
      m_clock(clock), m_camera(camera) {}

PlayStartResult PlaySession::start(SceneDocument document, const PropertyValidationContext& context,
                                   std::vector<std::unique_ptr<SimulationSystem>> systems, ClockSettings clock,
                                   PhysicsSettings physics) {
    for (const auto& system : systems)
        if (!system) return {nullptr, {}, "A play session cannot run an empty system"};
    auto camera = std::optional<EntityId>{};
    for (const auto& entity : document.entities)
        if (std::ranges::any_of(entity.components, [](const ComponentValue& value) { return std::holds_alternative<CameraComponent>(value); })) {
            camera = entity.id;
            break;
        }
    auto order = std::vector<EntityId>{};
    order.reserve(document.entities.size());
    for (const auto& entity : document.entities) order.push_back(entity.id);
    auto built = instantiate_scene(std::move(document), context);
    if (!built) return {nullptr, std::move(built.diagnostics), {}};
    // The scene's colliders and rigid bodies become bodies before any system starts, all or none.
    auto authored = authored_physics(*built.world, order, physics);
    if (!authored) return {nullptr, {}, "Physics: " + authored.error};
    auto session = std::unique_ptr<PlaySession>{};
    try {
        session.reset(new PlaySession(std::move(built.world), std::move(systems), clock, authored.settings, camera));
    } catch (const std::invalid_argument& error) {
        return {nullptr, {}, std::string("Physics could not start: ") + error.what()};
    }
    if (auto error = session->m_physics->create_bodies(*session->m_world, authored.bodies); !error.empty())
        return {nullptr, {}, "Physics: " + error};
    for (auto& system : session->m_systems) {
        ++session->m_started; // stop runs for a system whose start threw, too
        try {
            system->start(*session->m_world);
        } catch (const std::exception& error) {
            return {nullptr, {}, std::string(system->name()) + " could not start: " + error.what()};
        } catch (...) {
            return {nullptr, {}, std::string(system->name()) + " could not start"};
        }
    }
    return {std::move(session), {}, {}};
}

PlaySession::~PlaySession() {
    // Systems stop before the World they read goes away.
    while (m_started > 0) m_systems[--m_started]->stop();
}

PlayFrame PlaySession::update(double wall_delta) {
    auto frame = PlayFrame{};
    if (failed()) {
        frame.error = m_error;
        return frame;
    }
    frame.clock = m_clock.advance(wall_delta);
    for (uint32_t i = 0; i < frame.clock.ticks; ++i) {
        try {
            run_tick(frame.messages);
            ++frame.ticks_run;
        } catch (const std::exception& error) {
            m_error = "Tick " + std::to_string(m_clock.tick()) + ": " + error.what();
        } catch (...) {
            m_error = "Tick " + std::to_string(m_clock.tick()) + ": unknown error";
        }
        if (failed()) {
            frame.error = m_error;
            break;
        }
    }
    if (!failed()) {
        const auto admitted = std::isfinite(wall_delta) ? std::clamp(wall_delta, 0.0, m_clock.settings().max_frame_delta) : 0.0;
        auto context = FrameContext{*m_world, *m_physics, admitted, m_clock.alpha(), m_clock.tick(), m_clock.time(), frame.messages};
        for (auto& system : m_systems) {
            try {
                system->frame(context);
            } catch (const std::exception& error) {
                m_error = std::string(system->name()) + " failed after tick " + std::to_string(m_clock.tick()) + ": " + error.what();
                frame.error = m_error;
                break;
            }
        }
    }
    return frame;
}

// The fixed-tick phases of docs/architecture/scheduling-contracts.md. This tick's commit is the next
// tick's phase 1: the World batch, then bodies for destroyed entities go and requested ones arrive.
void PlaySession::run_tick(std::vector<SimulationMessage>& messages) {
    const auto input = m_input.latch(); // phase 2
    const auto interval = float(m_clock.interval());
    auto commands = m_world->commands();
    auto bodies = BodyCommands(*m_physics, *m_world);
    auto context = TickContext{*m_world, commands, input, m_clock.tick(), m_clock.time(), interval, bodies, *m_physics, messages};
    for (auto& system : m_systems) { // phase 3
        const auto first = commands.size();
        bodies.set_source(system->name());
        try {
            system->fixed_update(context);
            m_physics->check_world_commands(*m_world, commands, first);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(system->name()) + " failed: " + error.what());
        }
    }
    try {
        m_physics->prepare(*m_world, bodies, interval); // phase 4
        m_physics->step(interval); // phase 5
        m_physics->synchronize(*m_world, commands); // phase 6
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("Physics failed: ") + error.what());
    }
    // One atomic commit: a rejected batch leaves the World as the previous tick completed it.
    const auto result = m_world->commit(commands);
    if (!result)
        throw std::runtime_error("The World rejected the tick's changes at command " + std::to_string(result.command_index) +
                                 " (" + error_name(result.error) + ")");
    m_physics->commit(*m_world, bodies, result);
    m_clock.count_tick();
}

} // namespace maya
