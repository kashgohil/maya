#include "maya/simulation/animation.hpp"
#include "maya/world/components.hpp"
#include "maya/world/name_path.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace maya {
namespace {

/// A clip's channels grouped by the entity they move: each distinct target once, in first-channel order.
struct PreparedClip {
    std::shared_ptr<const AnimationAsset> clip;
    std::vector<std::string> targets;
    std::vector<uint32_t> channel_target; // per channel, its index in targets
};

class AnimationSystem final : public SimulationSystem {
public:
    explicit AnimationSystem(AnimationClips clips) : m_clips(std::move(clips)) {}
    std::string_view name() const override { return "Animation"; }

    void fixed_update(TickContext& tick) override {
        // In EntityId order, so two clips moving one entity always resolve the same way: the later wins.
        auto animated = std::vector<std::tuple<EntityId, EntityHandle, AnimationComponent>>{};
        tick.world.for_each<AnimationComponent>([&](EntityHandle entity, const AnimationComponent& component) {
            if (const auto id = tick.world.persistent_id(entity)) animated.emplace_back(*id, entity, component);
        });
        std::ranges::sort(animated, {}, [](const auto& item) { return std::get<0>(item); });
        auto seen = std::set<EntityId>{};
        for (const auto& [id, entity, component] : animated) {
            seen.insert(id);
            play(tick, id, entity, component);
        }
        std::erase_if(m_playing, [&](const auto& item) { return !seen.contains(item.first); });
    }

private:
    struct Playing {
        AssetId clip;
        float start = 0.0f;
        double time = 0.0; // where this tick samples
        std::optional<double> posed; // the time the joints were last written at
        // The clip's targets as last resolved, kept while the World's names and hierarchy are unchanged.
        std::vector<std::optional<EntityHandle>> targets;
        uint64_t names_revision = 0;
        bool bound = false;
    };

    void report(TickContext& tick, EntityHandle entity, std::string text, SimulationMessage::Level level = SimulationMessage::Level::warning) {
        auto label = std::string{};
        tick.world.with<NameComponent>(entity, [&](const NameComponent& name) { label = name.value; });
        if (label.empty()) label = "an entity";
        tick.messages.push_back({level, std::string(name()), "'" + label + "' " + std::move(text), tick.tick});
    }

    const PreparedClip* clip(TickContext& tick, EntityId id, EntityHandle entity, AssetId asset) {
        if (const auto found = m_prepared.find(asset); found != m_prepared.end()) {
            if (found->second.clip) return &found->second;
        } else {
            auto loaded = m_clips ? m_clips(asset) : AnimationClipResult{nullptr, "no clips are available to this session"};
            auto prepared = PreparedClip{};
            if (loaded.clip) {
                prepared.clip = loaded.clip;
                for (const auto& channel : loaded.clip->channels) {
                    const auto at = std::ranges::find(prepared.targets, channel.target);
                    prepared.channel_target.push_back(uint32_t(at - prepared.targets.begin()));
                    if (at == prepared.targets.end()) prepared.targets.push_back(channel.target);
                }
            }
            m_errors[asset] = loaded.error;
            const auto& stored = m_prepared.emplace(asset, std::move(prepared)).first->second;
            if (stored.clip) return &stored;
        }
        if (m_reported.insert({id, asset, UINT32_MAX}).second)
            report(tick, entity, "cannot play its clip: " + m_errors[asset]);
        return nullptr;
    }

    void play(TickContext& tick, EntityId id, EntityHandle entity, const AnimationComponent& component) {
        if (!component.clip.valid()) {
            m_playing.erase(id);
            return;
        }
        const auto* prepared = clip(tick, id, entity, component.clip.id);
        if (!prepared) return;
        const auto& asset = *prepared->clip;
        auto [found, fresh] = m_playing.try_emplace(id);
        auto& playing = found->second;
        // A new clip or start time begins again; its joints jump there, as they do on the first tick.
        const auto jump = fresh || playing.clip != component.clip.id || playing.start != component.start;
        if (jump) {
            playing = Playing{};
            playing.clip = component.clip.id;
            playing.start = component.start;
            playing.time = clip_time(asset.duration, component.start, component.loop);
        }
        if (!jump && playing.posed == playing.time) {
            advance(tick, playing, component, asset);
            return; // the joints already hold this pose
        }
        if (!playing.bound || playing.names_revision != tick.world.names_revision()) {
            playing.targets = resolve_name_paths(tick.world, entity, prepared->targets);
            playing.names_revision = tick.world.names_revision();
            playing.bound = true;
        }
        const auto& targets = playing.targets;
        auto poses = std::vector<std::optional<TransformComponent>>(targets.size());
        for (size_t t = 0; t < targets.size(); ++t) {
            if (targets[t]) tick.world.with<TransformComponent>(*targets[t], [&](const TransformComponent& value) { poses[t] = value; });
            else if (m_reported.insert({id, component.clip.id, uint32_t(t)}).second)
                report(tick, entity, "has no entity at '" + prepared->targets[t] + "', which its clip '" + asset.name +
                                         "' moves (renamed or removed?); those channels are skipped");
        }
        for (size_t c = 0; c < asset.channels.size(); ++c) {
            auto& pose = poses[prepared->channel_target[c]];
            if (!pose) continue;
            const auto& channel = asset.channels[c];
            const auto value = sample_channel(channel, float(playing.time));
            switch (channel.path) {
            case ChannelPath::translation: pose->translation = {value.x, value.y, value.z}; break;
            case ChannelPath::rotation: pose->rotation = {value.x, value.y, value.z, value.w}; break;
            case ChannelPath::scale: { // a zero scale, which hides, is held just above it: the World needs an inverse
                const auto shown = [](float s) { return s >= 0.0f && s < min_animated_scale ? min_animated_scale : s; };
                pose->scale = {shown(value.x), shown(value.y), shown(value.z)};
                break;
            }
            }
        }
        for (size_t t = 0; t < targets.size(); ++t) {
            if (!poses[t]) continue;
            // A pose the World cannot hold (a negative scale) keeps the entity's last one.
            const auto valid = validated_transform(*poses[t]);
            if (!valid) {
                if (m_reported.insert({id, component.clip.id, uint32_t(targets.size() + t)}).second)
                    report(tick, entity, "cannot pose '" + prepared->targets[t] + "' as its clip '" + asset.name +
                                             "' says (a negative scale?); it keeps its last pose then");
                continue;
            }
            tick.commands.set_transform(*targets[t], *valid);
            if (jump && tick.jumps) tick.jumps->push_back(*targets[t]);
        }
        playing.posed = playing.time;
        advance(tick, playing, component, asset);
    }

    static void advance(const TickContext& tick, Playing& playing, const AnimationComponent& component, const AnimationAsset& asset) {
        if (component.playing && component.speed != 0.0f)
            playing.time = clip_time(asset.duration, playing.time + double(tick.delta) * component.speed, component.loop);
    }

    AnimationClips m_clips;
    std::unordered_map<AssetId, PreparedClip, PersistentIdHash> m_prepared; // every clip asked for; empty when it failed
    std::unordered_map<AssetId, std::string, PersistentIdHash> m_errors;
    std::map<EntityId, Playing> m_playing;
    std::set<std::tuple<EntityId, AssetId, uint32_t>> m_reported; // problems reported once each
};

} // namespace

std::unique_ptr<SimulationSystem> animation_system(AnimationClips clips) {
    return std::make_unique<AnimationSystem>(std::move(clips));
}

} // namespace maya
