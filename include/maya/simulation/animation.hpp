#pragma once
// Skeletal animation in play (#1038, docs/animation.md#playing): the system that plays maya.animation
// components' clips by writing their joints' transforms each fixed tick.

#include "maya/assets/animation.hpp"
#include "maya/simulation/simulation.hpp"
#include <functional>
#include <memory>
#include <string>

namespace maya {

struct AnimationClipResult {
    std::shared_ptr<const AnimationAsset> clip; // null when it cannot be loaded
    std::string error; // why not
};
/// The least scale a clip gives an axis: glTF's zero scale, which hides what it scales, is held here, since
/// the World keeps only transforms it can invert. Negative scales are not shown (and are reported).
inline constexpr float min_animated_scale = 1e-6f;

/// Finds a clip by its asset ID; see registry_animation_clips in script_assets.hpp.
using AnimationClips = std::function<AnimationClipResult(AssetId)>;

/// The system that plays maya.animation components, after the scripts. Each tick it reads the components
/// as the tick began, so a script's change shows one tick later, and for each one samples its clip at the
/// clip's current time and writes the transforms of the entities its channels name, by their paths below
/// the animated entity. Then a playing clip's time advances by the tick times its speed. A new clip or start
/// time starts again at the start, and its joints jump there rather than blend (TickContext::jumps). A clip
/// that cannot be loaded, a channel whose entity is missing (renamed, removed, or never there), and a pose
/// the World cannot hold are reported once each and skipped; the rest of the clip plays.
std::unique_ptr<SimulationSystem> animation_system(AnimationClips clips);

} // namespace maya
