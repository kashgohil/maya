#include "maya/assets/animation.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace maya {
namespace {
math::Vec4 key_value(const AnimationChannel& channel, size_t key, size_t part) {
    // A cubic spline key holds an in-tangent, the value, and an out-tangent: parts 0, 1, and 2.
    const auto width = channel_width(channel.path);
    const auto stride = channel.interpolation == Interpolation::cubic_spline ? 3 * width : width;
    const auto* v = channel.values.data() + key * stride + (channel.interpolation == Interpolation::cubic_spline ? part * width : 0);
    return width == 4 ? math::Vec4{v[0], v[1], v[2], v[3]} : math::Vec4{v[0], v[1], v[2], 0.0f};
}
math::Vec4 normalized(math::Vec4 q) {
    const auto length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return length > 0 ? q * (1.0f / length) : math::Vec4{0, 0, 0, 1};
}
math::Vec4 slerp(math::Vec4 a, math::Vec4 b, float t) {
    auto cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cosine < 0) { // the shorter way round
        b = b * -1.0f;
        cosine = -cosine;
    }
    if (cosine > 0.9995f) return normalized(a + (b - a) * t); // nearly the same: linear is exact enough
    const auto angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
    const auto sine = std::sin(angle);
    return normalized(a * (std::sin((1 - t) * angle) / sine) + b * (std::sin(t * angle) / sine));
}
template<class T> void put(std::vector<std::byte>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
template<class T> bool take(std::span<const std::byte>& in, T& value) {
    if (in.size() < sizeof(T)) return false;
    std::memcpy(&value, in.data(), sizeof(T));
    in = in.subspan(sizeof(T));
    return true;
}
void put_text(std::vector<std::byte>& out, const std::string& text) {
    put(out, uint32_t(text.size()));
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    out.insert(out.end(), bytes, bytes + text.size());
}
bool take_text(std::span<const std::byte>& in, std::string& text) {
    uint32_t size = 0;
    if (!take(in, size) || in.size() < size) return false;
    text.assign(reinterpret_cast<const char*>(in.data()), size);
    in = in.subspan(size);
    return true;
}
template<class T> void put_array(std::vector<std::byte>& out, const std::vector<T>& values) {
    put(out, uint64_t(values.size()));
    const auto* bytes = reinterpret_cast<const std::byte*>(values.data());
    out.insert(out.end(), bytes, bytes + values.size() * sizeof(T));
}
template<class T> bool take_array(std::span<const std::byte>& in, std::vector<T>& values) {
    uint64_t count = 0;
    if (!take(in, count) || count > in.size() / sizeof(T)) return false;
    values.resize(count);
    std::memcpy(values.data(), in.data(), count * sizeof(T));
    in = in.subspan(count * sizeof(T));
    return true;
}
} // namespace

std::string check_channel(const AnimationChannel& channel) {
    if (channel.times.empty()) return "has no keys";
    const auto width = channel_width(channel.path) * (channel.interpolation == Interpolation::cubic_spline ? 3 : 1);
    if (channel.values.size() != channel.times.size() * width)
        return "has " + std::to_string(channel.values.size()) + " values for " + std::to_string(channel.times.size()) + " keys";
    for (size_t i = 0; i < channel.times.size(); ++i) {
        if (!std::isfinite(channel.times[i]) || channel.times[i] < 0) return "key " + std::to_string(i) + "'s time is not a finite, nonnegative number";
        if (i > 0 && channel.times[i] <= channel.times[i - 1]) return "key " + std::to_string(i) + "'s time does not increase";
    }
    if (std::ranges::any_of(channel.values, [](float v) { return !std::isfinite(v); })) return "has a value that is not finite";
    return {};
}

math::Vec4 sample_channel(const AnimationChannel& channel, float time) {
    const auto& times = channel.times;
    const auto rotation = channel.path == ChannelPath::rotation;
    const auto finish = [&](math::Vec4 value) { return rotation ? normalized(value) : value; };
    if (time <= times.front()) return finish(key_value(channel, 0, 1));
    if (time >= times.back()) return finish(key_value(channel, times.size() - 1, 1));
    const auto next = size_t(std::ranges::upper_bound(times, time) - times.begin());
    const auto key = next - 1;
    const auto span = times[next] - times[key];
    const auto t = (time - times[key]) / span;
    switch (channel.interpolation) {
    case Interpolation::step: return finish(key_value(channel, key, 1));
    case Interpolation::linear: {
        const auto a = key_value(channel, key, 1), b = key_value(channel, next, 1);
        return rotation ? slerp(a, b, t) : a + (b - a) * t;
    }
    case Interpolation::cubic_spline: {
        // glTF's Hermite spline: p(t) = (2t^3 - 3t^2 + 1) v0 + (t^3 - 2t^2 + t) d m0 + (-2t^3 + 3t^2) v1 + (t^3 - t^2) d m1,
        // with the out-tangent of the key before and the in-tangent of the key after, scaled by the span d.
        const auto t2 = t * t, t3 = t2 * t;
        const auto v0 = key_value(channel, key, 1), m0 = key_value(channel, key, 2) * span;
        const auto v1 = key_value(channel, next, 1), m1 = key_value(channel, next, 0) * span;
        return finish(v0 * (2 * t3 - 3 * t2 + 1) + m0 * (t3 - 2 * t2 + t) + v1 * (-2 * t3 + 3 * t2) + m1 * (t3 - t2));
    }
    }
    return finish(key_value(channel, key, 1));
}

double clip_time(double duration, double t, bool loop) noexcept {
    if (!(duration > 0)) return 0;
    if (loop) {
        const auto wrapped = std::fmod(t, duration);
        return wrapped < 0 ? wrapped + duration : wrapped;
    }
    return std::clamp(t, 0.0, duration);
}

std::vector<std::byte> write_skin(const SkinAsset& skin) {
    auto out = std::vector<std::byte>{};
    put(out, uint32_t(skin.joints.size()));
    for (const auto& joint : skin.joints) put_text(out, joint);
    put_array(out, skin.inverse_bind);
    return out;
}
std::optional<SkinAsset> read_skin(std::span<const std::byte> in) {
    auto skin = SkinAsset{};
    uint32_t joints = 0;
    if (!take(in, joints) || joints > in.size() / sizeof(uint32_t)) return std::nullopt;
    skin.joints.resize(joints);
    for (auto& joint : skin.joints)
        if (!take_text(in, joint)) return std::nullopt;
    if (!take_array(in, skin.inverse_bind) || !in.empty() || skin.inverse_bind.size() != skin.joints.size()) return std::nullopt;
    return skin;
}
std::vector<std::byte> write_animation(const AnimationAsset& clip) {
    auto out = std::vector<std::byte>{};
    put_text(out, clip.name);
    put(out, clip.duration);
    put(out, uint32_t(clip.channels.size()));
    for (const auto& channel : clip.channels) {
        put_text(out, channel.target);
        put(out, uint8_t(channel.path));
        put(out, uint8_t(channel.interpolation));
        put_array(out, channel.times);
        put_array(out, channel.values);
    }
    return out;
}
std::optional<AnimationAsset> read_animation(std::span<const std::byte> in) {
    auto clip = AnimationAsset{};
    uint32_t channels = 0;
    if (!take_text(in, clip.name) || !take(in, clip.duration) || !take(in, channels) || channels > in.size()) return std::nullopt;
    clip.channels.resize(channels);
    for (auto& channel : clip.channels) {
        uint8_t path = 0, interpolation = 0;
        if (!take_text(in, channel.target) || !take(in, path) || !take(in, interpolation) || path > 2 || interpolation > 2) return std::nullopt;
        channel.path = ChannelPath(path);
        channel.interpolation = Interpolation(interpolation);
        if (!take_array(in, channel.times) || !take_array(in, channel.values) || !check_channel(channel).empty()) return std::nullopt;
    }
    if (!in.empty() || !std::isfinite(clip.duration) || clip.duration < 0) return std::nullopt;
    return clip;
}

} // namespace maya
