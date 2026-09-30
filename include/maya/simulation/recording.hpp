#pragma once

#include "maya/simulation/simulation.hpp"
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

// Play recordings (docs/play.md#recording-and-replay): a session's per-tick input with everything its
// result depends on, so a replay on the same machine and build gives the same result.
namespace maya {

bool same_input(const InputFrame& a, const InputFrame& b) noexcept;

/// Per-tick input, stored only where it changes: a held key costs one entry, not one per tick.
class InputTrack {
public:
    struct Change {
        uint64_t tick = 0;
        InputFrame frame;
    };
    /// The next tick's input.
    void push(const InputFrame& frame);
    /// Tick `tick`'s input; ticks past the end repeat the last frame.
    InputFrame at(uint64_t tick) const;
    uint64_t size() const noexcept { return m_ticks; }
    const std::vector<Change>& changes() const noexcept { return m_changes; }
    /// For reading files: a change at `tick`, after the previous one, and the track's length.
    bool add_change(uint64_t tick, InputFrame frame);
    bool set_size(uint64_t ticks);

private:
    std::vector<Change> m_changes;
    uint64_t m_ticks = 0;
};

/// A state_hash (PlaySession) after a tick completed.
struct PlayCheckpoint {
    uint64_t tick = 0; // ticks completed
    uint64_t state = 0;
    bool operator==(const PlayCheckpoint&) const = default;
};
inline constexpr uint32_t recording_checkpoint_interval = 60; // ticks between checkpoints

/// An asset the scene plays with, by the hash of what the session used: a script's source text, or
/// an asset file's bytes.
struct RecordedAsset {
    AssetId id;
    std::string kind; // "mesh", "material", or "script"
    std::string path; // in its project
    std::string hash; // 16 hexadecimal digits (FNV-1a 64)
    bool operator==(const RecordedAsset&) const = default;
};
/// A script reload applied during the recorded session; its replay is not promised to match.
struct RecordedReload {
    uint64_t tick = 0;
    AssetId script;
    std::string name;
    bool operator==(const RecordedReload&) const = default;
};

struct PlayRecording {
    static constexpr uint32_t format_version = 1;
    std::string build; // recording_build() of the build that recorded it
    std::string physics; // recording_physics() of its session
    uint64_t seed = 0; // the scripts' seed (ScriptSettings::seed)
    uint32_t ticks_per_second = 60;
    std::string scene_name; // the scene's path in its project, for messages
    std::string scene; // the scene document as scene-file text: what was played, saved or not
    std::vector<RecordedAsset> assets;
    std::vector<RecordedReload> reloads;
    InputTrack inputs;
    std::vector<PlayCheckpoint> checkpoints;
    uint64_t final_state = 0; // full_state_hash after the last tick
};

/// This build: its revision (with a hash of uncommitted changes), type, sanitizers, and compiler.
std::string recording_build();
/// The physics a session runs with: the Jolt configuration and the settings that change results.
std::string recording_physics(const PhysicsSettings& settings);
/// 16 hexadecimal digits of FNV-1a 64 over `bytes`.
std::string content_hash(std::string_view bytes);

/// Writes the versioned text format ("maya-recording 1").
void write_recording(std::ostream& output, const PlayRecording& recording);
struct RecordingReadResult {
    std::optional<PlayRecording> recording;
    std::string error; // why the file is not a recording this build reads
    explicit operator bool() const noexcept { return recording.has_value(); }
};
RecordingReadResult read_recording(std::istream& input);

/// Why `recording` cannot be replayed by this build with these assets (`assets` as the project has
/// them now) and physics settings, or empty. Another build or Jolt configuration, a changed or missing
/// asset, and a session with a script reload are each refused: a replay would not be promised to match.
std::string replay_refusal(const PlayRecording& recording, const std::vector<RecordedAsset>& assets, const PhysicsSettings& settings);

} // namespace maya
