#include "maya/simulation/simulation.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace maya {

FixedClock::FixedClock(ClockSettings settings) : m_settings(settings) {
    if (settings.ticks_per_second == 0 || settings.max_catch_up_ticks == 0 ||
        !std::isfinite(settings.max_frame_delta) || settings.max_frame_delta <= 0.0)
        throw std::invalid_argument("Clock settings need a positive tick rate, catch-up cap, and frame delta cap");
}

ClockAdvance FixedClock::advance(double wall_delta) {
    auto result = ClockAdvance{};
    if (!std::isfinite(wall_delta) || wall_delta < 0.0) {
        result.invalid_delta = true;
        wall_delta = 0.0;
    }
    if (m_paused) { // paused wall time is not admitted; a requested step runs one tick
        if (std::exchange(m_step, false)) result.ticks = 1;
        return result;
    }
    if (wall_delta > m_settings.max_frame_delta) {
        result.rejected_time = wall_delta - m_settings.max_frame_delta;
        wall_delta = m_settings.max_frame_delta;
    }
    m_accumulator += wall_delta;
    const auto due = uint64_t(std::floor(m_accumulator / interval()));
    m_accumulator = std::max(0.0, m_accumulator - double(due) * interval());
    result.ticks = uint32_t(std::min<uint64_t>(due, m_settings.max_catch_up_ticks));
    result.discarded_ticks = uint32_t(due - result.ticks);
    m_rejected += result.rejected_time;
    m_discarded += result.discarded_ticks;
    return result;
}

void FixedClock::resume() noexcept {
    if (!m_paused) return;
    m_paused = false;
    m_step = false;
    m_accumulator = 0.0; // no catch-up for the time spent paused
}

void FixedClock::step() noexcept {
    if (m_paused) m_step = true;
}

float FixedClock::alpha() const noexcept {
    if (m_paused) return 1.0f;
    return std::clamp(float(m_accumulator / interval()), 0.0f, std::nextafter(1.0f, 0.0f));
}

bool InputFrame::down(KeyCode key) const noexcept {
    const auto code = static_cast<int>(key);
    return code >= 0 && size_t(code) < key_count && held.test(size_t(code));
}
bool InputFrame::went_down(KeyCode key) const noexcept {
    const auto code = static_cast<int>(key);
    return code >= 0 && size_t(code) < key_count && pressed.test(size_t(code));
}
bool InputFrame::went_up(KeyCode key) const noexcept {
    const auto code = static_cast<int>(key);
    return code >= 0 && size_t(code) < key_count && released.test(size_t(code));
}

void GameInput::feed(const std::vector<InputEvent>& events) {
    const auto change = [](auto& held, auto& pressed, auto& released, size_t index, bool down) {
        if (down && !held.test(index)) pressed.set(index); // repeats of a held key are not new presses
        if (!down && held.test(index)) released.set(index);
        held.set(index, down);
    };
    for (const auto& event : events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, KeyEvent>) {
                const auto code = static_cast<int>(e.key);
                if (code >= 0 && size_t(code) < InputFrame::key_count)
                    change(m_pending.held, m_pending.pressed, m_pending.released, size_t(code), e.down);
            } else if constexpr (std::is_same_v<T, MouseButtonEvent>) {
                change(m_pending.buttons_held, m_pending.buttons_pressed, m_pending.buttons_released,
                       static_cast<size_t>(e.button), e.down);
            } else if constexpr (std::is_same_v<T, MouseMoveEvent>) {
                if (m_pointer) m_pending.look += math::Vec2{e.x - m_pointer->x, e.y - m_pointer->y};
                m_pointer = math::Vec2{e.x, e.y};
            } else if constexpr (std::is_same_v<T, ScrollEvent>) {
                m_pending.scroll += e.y;
            } else if constexpr (std::is_same_v<T, FocusEvent>) {
                if (!e.focused) release_all();
            }
        }, event);
    }
}

InputFrame GameInput::latch() {
    auto frame = m_pending;
    m_pending.pressed.reset();
    m_pending.released.reset();
    m_pending.buttons_pressed.reset();
    m_pending.buttons_released.reset();
    m_pending.look = {0.0f, 0.0f};
    m_pending.scroll = 0.0f;
    return frame;
}

void GameInput::release_all() {
    m_pending.released |= m_pending.held;
    m_pending.held.reset();
    m_pending.buttons_released |= m_pending.buttons_held;
    m_pending.buttons_held.reset();
    m_pointer.reset(); // the next movement starts a new baseline, not a jump
}

namespace {

/// maya.spin: turns about the local axis at a constant rate.
class SpinSystem final : public SimulationSystem {
public:
    std::string_view name() const override { return "Spin"; }
    void fixed_update(TickContext& tick) override {
        tick.world.for_each<TransformComponent, SpinComponent>(
            [&](EntityHandle entity, const TransformComponent& transform, const SpinComponent& spin) {
                const auto length = spin.axis.length();
                if (!(length > 1e-6f) || spin.speed == 0.0f) return;
                auto turned = transform;
                turned.rotation = transform.rotation * math::Quat::from_axis_angle(spin.axis * (1.0f / length), spin.speed * tick.delta);
                turned.rotation.normalize();
                tick.commands.set_transform(entity, turned);
            });
    }
};

/// maya.fly_control: moves and turns the entity from gameplay input, in its parent's space. Turning
/// is yaw about +Y and pitch about the local X axis, taken from the authored rotation the first time
/// the entity turns (roll is dropped then). Pitch stops just short of straight up or down.
class FlyControlSystem final : public SimulationSystem {
public:
    std::string_view name() const override { return "Fly control"; }
    void fixed_update(TickContext& tick) override {
        const auto& input = tick.input;
        const auto axis = [&](KeyCode positive, KeyCode negative) {
            return (input.down(positive) ? 1.0f : 0.0f) - (input.down(negative) ? 1.0f : 0.0f);
        };
        const auto forward = axis(KeyCode::W, KeyCode::S), right = axis(KeyCode::D, KeyCode::A), up = axis(KeyCode::E, KeyCode::Q);
        const auto turning = input.look.x != 0.0f || input.look.y != 0.0f;
        if (!turning && forward == 0.0f && right == 0.0f && up == 0.0f) return;
        const auto fast = input.down(KeyCode::LeftShift) || input.down(KeyCode::RightShift);
        tick.world.for_each<TransformComponent, FlyControlComponent>(
            [&](EntityHandle entity, const TransformComponent& transform, const FlyControlComponent& control) {
                auto moved = transform;
                if (turning) {
                    auto& [yaw, pitch] = m_angles.try_emplace(entity.slot, angles(transform.rotation)).first->second;
                    constexpr auto limit = math::PI * 0.5f - 0.01f;
                    yaw -= input.look.x * control.look_sensitivity;
                    pitch = std::clamp(pitch - input.look.y * control.look_sensitivity, -limit, limit);
                    moved.rotation = math::Quat::from_axis_angle({0.0f, 1.0f, 0.0f}, yaw) *
                                     math::Quat::from_axis_angle({1.0f, 0.0f, 0.0f}, pitch);
                    moved.rotation.normalize();
                }
                const auto step = control.speed * (fast ? 4.0f : 1.0f) * tick.delta;
                moved.translation += (moved.rotation.rotate({0.0f, 0.0f, -1.0f}) * forward +
                                      moved.rotation.rotate({1.0f, 0.0f, 0.0f}) * right +
                                      math::Vec3{0.0f, 1.0f, 0.0f} * up) * step;
                tick.commands.set_transform(entity, moved);
            });
    }
    void stop() noexcept override { m_angles.clear(); }

private:
    static std::pair<float, float> angles(const math::Quat& rotation) {
        const auto look = rotation.rotate({0.0f, 0.0f, -1.0f});
        return {std::atan2(-look.x, -look.z), std::asin(std::clamp(look.y, -1.0f, 1.0f))};
    }
    // Keyed by slot: the session's World never reuses a slot for another entity while it plays.
    std::unordered_map<uint32_t, std::pair<float, float>> m_angles;
};

} // namespace

std::vector<std::unique_ptr<SimulationSystem>> builtin_systems() {
    auto systems = std::vector<std::unique_ptr<SimulationSystem>>{};
    systems.push_back(std::make_unique<FlyControlSystem>());
    systems.push_back(std::make_unique<SpinSystem>());
    return systems;
}

} // namespace maya
