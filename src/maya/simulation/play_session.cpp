#include "maya/simulation/play_session.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace maya {

PlaySession::PlaySession(std::unique_ptr<World> world, std::vector<std::unique_ptr<SimulationSystem>> systems,
                         ClockSettings clock, std::optional<EntityId> camera)
    : m_world(std::move(world)), m_systems(std::move(systems)), m_clock(clock), m_camera(camera) {}

PlayStartResult PlaySession::start(SceneDocument document, const PropertyValidationContext& context,
                                   std::vector<std::unique_ptr<SimulationSystem>> systems, ClockSettings clock) {
    for (const auto& system : systems)
        if (!system) return {nullptr, {}, "A play session cannot run an empty system"};
    auto camera = std::optional<EntityId>{};
    for (const auto& entity : document.entities)
        if (std::ranges::any_of(entity.components, [](const ComponentValue& value) { return std::holds_alternative<CameraComponent>(value); })) {
            camera = entity.id;
            break;
        }
    auto built = instantiate_scene(std::move(document), context);
    if (!built) return {nullptr, std::move(built.diagnostics), {}};
    auto session = std::unique_ptr<PlaySession>(new PlaySession(std::move(built.world), std::move(systems), clock, camera));
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
            run_tick();
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
    return frame;
}

void PlaySession::run_tick() {
    const auto input = m_input.latch();
    auto commands = m_world->commands();
    auto context = TickContext{*m_world, commands, input, m_clock.tick(), m_clock.time(), float(m_clock.interval())};
    for (auto& system : m_systems) {
        try {
            system->fixed_update(context);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(system->name()) + " failed: " + error.what());
        }
    }
    // One atomic commit: a rejected batch leaves the World as the previous tick completed it.
    if (const auto result = m_world->commit(commands); !result)
        throw std::runtime_error("The World rejected the tick's changes at command " + std::to_string(result.command_index) +
                                 " (" + error_name(result.error) + ")");
    m_clock.count_tick();
}

} // namespace maya
