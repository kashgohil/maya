#include "maya/simulation/play_session.hpp"
#include "maya/simulation/authored_physics.hpp"
#include "maya/world/spatial.hpp"
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
    end_contacts();
    // Systems stop before the World they read goes away.
    while (m_started > 0) m_systems[--m_started]->stop();
}

void PlaySession::end_contacts() noexcept {
    // Every contact and trigger still in progress ends, marked as removed; nothing systems do is kept.
    if (failed() || m_started < m_systems.size()) return;
    try {
        const auto events = m_physics->end_contacts(*m_world, m_clock.tick());
        auto discarded = m_world->commands();
        auto bodies = BodyCommands(*m_physics, *m_world);
        auto messages = std::vector<SimulationMessage>{};
        const auto input = InputFrame{};
        auto context = TickContext{*m_world, discarded, input, m_clock.tick(), m_clock.time(), float(m_clock.interval()),
                                   bodies, *m_physics, messages, events, true};
        for (auto& system : m_systems) {
            try {
                system->late_fixed_update(context);
            } catch (...) {
            }
        }
    } catch (...) {
    }
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
        // Phase 4: the requests of the last phase 7, then this tick's.
        auto lists = std::vector<const BodyCommands*>{};
        if (m_late_bodies) lists.push_back(m_late_bodies.get());
        lists.push_back(&bodies);
        m_physics->prepare(*m_world, lists, interval);
        m_physics->step(interval); // phase 5
        m_physics->synchronize(*m_world, commands); // phase 6
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("Physics failed: ") + error.what());
    }
    // Phase 7: the step's events, then late_fixed_update. Changes join this tick's batch; body
    // requests wait for the next step, so the completed one cannot change.
    const auto events = m_physics->take_events(*m_world, commands, m_clock.tick());
    auto late = std::make_unique<BodyCommands>(*m_physics, *m_world);
    auto late_context = TickContext{*m_world, commands, input, m_clock.tick(), m_clock.time(), interval, *late, *m_physics, messages, events};
    for (auto& system : m_systems) {
        const auto first = commands.size();
        late->set_source(system->name());
        try {
            system->late_fixed_update(late_context);
            m_physics->check_world_commands(*m_world, commands, first);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(system->name()) + " failed after the step: " + error.what());
        }
    }
    // Phase 2's pose history, kept as the batch commits: the poses this tick changes, and which reset.
    const BodyCommands* stepped[] = {m_late_bodies ? m_late_bodies.get() : &bodies, &bodies};
    keep_history(commands, stepped);
    // One atomic commit: a rejected batch leaves the World as the previous tick completed it.
    const auto result = m_world->commit(commands);
    if (!result)
        throw std::runtime_error("The World rejected the tick's changes at command " + std::to_string(result.command_index) +
                                 " (" + error_name(result.error) + ")");
    reset_history(m_resets);
    m_physics->commit(*m_world, bodies, result);
    m_physics->commit(*m_world, *late, result); // bodies created or removed in phase 7
    m_late_bodies = std::move(late);
    m_clock.count_tick();
}

} // namespace maya

namespace maya {

// Before the commit: each entity's local transform that the batch changes, as it was. Entities the
// batch creates have none, so they show their first pose as it is. Reparented and teleported
// entities (teleports that took effect in this tick's step) reset, with their descendants.
void PlaySession::keep_history(const WorldCommands& commands, std::span<const BodyCommands* const> stepped) {
    for (const auto& entry : m_previous) m_history_index[entry.entity.slot] = 0;
    m_previous.clear();
    m_resets.clear();
    using Kind = WorldCommands::Kind;
    commands.for_each_staged(0, [&](const WorldCommands::Staged& staged) {
        const auto* entity = std::get_if<EntityHandle>(&staged.target);
        if (!entity || !m_world->alive(*entity)) return;
        if (staged.kind == Kind::reparent) m_resets.push_back(*entity);
        const auto transform = staged.kind == Kind::set_transform ||
                               ((staged.kind == Kind::replace || staged.kind == Kind::add) && staged.component == typeid(TransformComponent));
        if (!transform) return;
        if (entity->slot >= m_history_index.size()) m_history_index.resize(size_t(entity->slot) + 1, 0);
        if (m_history_index[entity->slot] != 0) return; // already kept: its pose before the batch
        m_world->with<TransformComponent>(*entity, [&](const TransformComponent& value) {
            m_previous.push_back({*entity, value});
            m_history_index[entity->slot] = uint32_t(m_previous.size());
        });
    });
    for (size_t i = 0; i < stepped.size(); ++i)
        if (i == 0 || stepped[i] != stepped[0])
            for (const auto entity : stepped[i]->teleports()) m_resets.push_back(entity);
}

// After the commit: reset entities, and everything below them, show their current pose.
void PlaySession::reset_history(const std::vector<EntityHandle>& entities) {
    const auto kept = [&](EntityHandle entity) -> History* {
        const auto index = entity.slot < m_history_index.size() ? m_history_index[entity.slot] : 0;
        return index != 0 && m_previous[index - 1].entity == entity ? &m_previous[index - 1] : nullptr;
    };
    const auto reset = [&](auto&& self, EntityHandle entity) -> void {
        if (auto* entry = kept(entity)) entry->reset = true;
        for (const auto child : m_world->children(entity)) self(self, child);
    };
    for (const auto entity : entities)
        if (m_world->alive(entity)) reset(reset, entity);
    for (auto& entry : m_previous)
        if (!m_world->alive(entry.entity)) entry.reset = true;
}

PresentationPoses PlaySession::presentation() const {
    auto poses = PresentationPoses{};
    const auto alpha = float(m_clock.alpha());
    if (!(alpha < 1.0f) || m_previous.empty()) return poses; // paused, or at a completed tick
    // The local poses between the last two ticks, for entities that moved (aligned with m_previous).
    auto between = std::vector<std::optional<TransformComponent>>(m_previous.size());
    auto moved = size_t{0};
    for (size_t i = 0; i < m_previous.size(); ++i) {
        const auto& entry = m_previous[i];
        if (entry.reset) continue;
        m_world->with<TransformComponent>(entry.entity, [&](const TransformComponent& now) {
            const auto& before = entry.before;
            const auto same = before.translation.x == now.translation.x && before.translation.y == now.translation.y &&
                              before.translation.z == now.translation.z && before.rotation.x == now.rotation.x &&
                              before.rotation.y == now.rotation.y && before.rotation.z == now.rotation.z &&
                              before.rotation.w == now.rotation.w && before.scale.x == now.scale.x &&
                              before.scale.y == now.scale.y && before.scale.z == now.scale.z;
            if (same) return;
            between[i] = interpolate_transform(before, now, alpha);
            ++moved;
        });
    }
    if (moved == 0) return poses;
    poses.reserve(moved);
    const auto shown_local = [&](EntityHandle entity) -> const std::optional<TransformComponent>* {
        const auto index = entity.slot < m_history_index.size() ? m_history_index[entity.slot] : 0;
        return index != 0 && m_previous[index - 1].entity == entity && between[index - 1] ? &between[index - 1] : nullptr;
    };
    // World matrices composed down the hierarchy from each moved entity, never by blending matrices:
    // a child keeps its place relative to its parent's shown pose.
    const auto descend = [&](auto&& self, EntityHandle entity, const math::Mat4& parent) -> void {
        auto local = std::optional<TransformComponent>{};
        if (const auto* found = shown_local(entity)) local = **found;
        else m_world->with<TransformComponent>(entity, [&](const TransformComponent& value) { local = value; });
        if (!local) return;
        const auto shown = compose_affine(parent, local_matrix(*local));
        if (!shown) return;
        poses.set(entity, *shown);
        for (const auto child : m_world->children(entity)) self(self, child, *shown);
    };
    for (size_t i = 0; i < m_previous.size(); ++i) {
        if (!between[i]) continue;
        const auto entity = m_previous[i].entity;
        const auto parent = m_world->parent(entity);
        if (!parent) {
            // A root's shown pose is its local one; both ends were valid poses, so the one between is.
            const auto shown = local_matrix(*between[i]);
            poses.set(entity, shown);
            for (const auto child : m_world->children(entity)) descend(descend, child, shown);
            continue;
        }
        auto ancestor_moved = false; // then it is reached from that ancestor
        for (auto above = parent; above && !ancestor_moved; above = m_world->parent(*above)) ancestor_moved = shown_local(*above) != nullptr;
        if (ancestor_moved) continue;
        if (const auto parent_world = m_world->world_matrix(*parent)) descend(descend, entity, *parent_world);
    }
    return poses;
}

} // namespace maya
