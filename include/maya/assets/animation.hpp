#pragma once
// Skeletal animation's assets (#1038, docs/animation.md): skins and clips, imported from glTF and cooked.
// Both name joints by their path of entity names below a root (e.g. "Armature/Hips/Spine"), so they bind
// to whatever entities carry those names, wherever the scene is placed.

#include "maya/math/matrix.hpp"
#include "maya/math/quaternion.hpp"
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace maya {

/// A skin: the joints a skinned mesh's vertices name, by path below the root they are bound from, and each
/// joint's inverse bind matrix (from the mesh's space to the joint's, in the bind pose).
struct SkinAsset {
    std::vector<std::string> joints;
    std::vector<math::Mat4> inverse_bind;
};

enum class ChannelPath : uint8_t { translation, rotation, scale };
/// glTF's samplers: hold each key until the next, interpolate linearly (slerp for rotations), or follow
/// a cubic Hermite spline with tangents stored beside each key.
enum class Interpolation : uint8_t { step, linear, cubic_spline };
struct AnimationChannel {
    std::string target; // the joint's path below the animated entity
    ChannelPath path = ChannelPath::translation;
    Interpolation interpolation = Interpolation::linear;
    std::vector<float> times; // seconds, increasing
    /// Per key: 3 floats (translation, scale) or 4 (rotation, x y z w); cubic splines store an in-tangent,
    /// the value, and an out-tangent per key.
    std::vector<float> values;
};
/// A clip: channels that move joints, and how long it lasts (its last key).
struct AnimationAsset {
    std::string name;
    float duration = 0;
    std::vector<AnimationChannel> channels;
};

/// The number of floats one key's value takes for a path: 3, or 4 for rotations.
constexpr size_t channel_width(ChannelPath path) noexcept { return path == ChannelPath::rotation ? 4 : 3; }
/// Why a channel cannot be sampled (keys out of order, the wrong number of values, not finite), or empty.
std::string check_channel(const AnimationChannel& channel);
/// A channel's value at `time` (seconds), held at its first and last keys: xyz for translation and scale,
/// a unit quaternion (x, y, z, w) for rotations.
math::Vec4 sample_channel(const AnimationChannel& channel, float time);

/// A time in a clip that lasts `duration` seconds: wrapped into [0, duration) when looping, held at the
/// ends otherwise. Play accumulates it in double precision, so a clip's end is where its ticks say.
double clip_time(double duration, double time, bool loop) noexcept;

/// Cooked forms, as the cook cache and packages store them.
std::vector<std::byte> write_skin(const SkinAsset& skin);
std::optional<SkinAsset> read_skin(std::span<const std::byte> bytes);
std::vector<std::byte> write_animation(const AnimationAsset& clip);
std::optional<AnimationAsset> read_animation(std::span<const std::byte> bytes);

} // namespace maya
