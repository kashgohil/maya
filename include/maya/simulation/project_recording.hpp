#pragma once

#include "maya/assets/registry.hpp"
#include "maya/scene/scene_io.hpp"
#include "maya/simulation/play_session.hpp"
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

// Recordings of a project's play sessions (docs/play.md#recording-and-replay): what the player and the
// editor share to record and replay, over MayaSimulation's recording.hpp.
namespace maya {

/// The assets `document` plays with, hashed: each mesh, material, and script its components name,
/// by the contents of its file. `script_text` gives a script's source when the session plays another
/// version than its file (the editor plays each script's last version that compiled).
inline std::vector<RecordedAsset> recorded_assets(const AssetRegistry& registry, const SceneDocument& document,
                                                  const std::function<std::optional<std::string>(AssetId)>& script_text = {}) {
    auto named = std::map<AssetId, AssetKind>{};
    for (const auto& entity : document.entities)
        for (const auto& component : entity.components) {
            if (const auto* renderer = std::get_if<MeshRendererComponent>(&component)) {
                if (renderer->mesh.valid()) named.emplace(renderer->mesh.id, AssetKind::mesh);
                if (renderer->material.valid()) named.emplace(renderer->material.id, AssetKind::material);
            } else if (const auto* script = std::get_if<ScriptComponent>(&component)) {
                if (script->script.valid()) named.emplace(script->script.id, AssetKind::script);
            }
        }
    auto assets = std::vector<RecordedAsset>{};
    for (const auto& [id, kind] : named) {
        auto asset = RecordedAsset{id, asset_kind_name(kind), "(not in the catalog)", "missing"};
        if (const auto info = registry.info(id)) {
            asset.path = info->record.path.generic_string();
            auto text = kind == AssetKind::script && script_text ? script_text(id) : std::nullopt;
            if (!text) {
                auto file = std::ifstream(registry.root() / info->record.path, std::ios::binary);
                if (file) text = std::string(std::istreambuf_iterator<char>(file), {});
            }
            if (text) asset.hash = content_hash(*text);
        }
        assets.push_back(std::move(asset));
    }
    return assets;
}

/// A recording's opening: what a session that plays `document` depends on. Its ticks come from the
/// session (finish_recording).
inline PlayRecording begin_recording(std::string scene_name, const SceneDocument& document, std::vector<RecordedAsset> assets,
                                     uint64_t seed, const ClockSettings& clock, const PhysicsSettings& physics) {
    auto recording = PlayRecording{};
    recording.build = recording_build();
    recording.physics = recording_physics(physics);
    recording.seed = seed;
    recording.ticks_per_second = clock.ticks_per_second;
    recording.scene_name = std::move(scene_name);
    const auto any_asset = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
    auto text = std::ostringstream{};
    write_scene(text, document, any_asset);
    recording.scene = text.str();
    recording.assets = std::move(assets);
    return recording;
}

/// Completes a recording from the session that made it: its input, checkpoints, and final state.
inline void finish_recording(PlayRecording& recording, const PlaySession& session) {
    recording.inputs = session.recorded_inputs();
    recording.checkpoints = session.recorded_checkpoints();
    recording.final_state = session.full_state_hash();
}

} // namespace maya
