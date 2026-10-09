#pragma once
// The cook cache (#1036, docs/assets.md#cook-cache): what loading cooks from a source (textures, their
// mips and ASTC blocks; environments; imported meshes) is kept in the project's .maya/cache folder, keyed
// by the source's bytes and the settings that change the result, so a later load reads it instead of
// cooking again. Deleting the folder is always safe; it is never shared through version control.

#include "maya/core/sha256.hpp"
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace maya {
struct Project;

/// Versions of what each kind of entry holds. A change to how something is cooked that changes its
/// bytes must raise its version, so entries cooked before are not read.
inline constexpr uint32_t texture_cook_version = 1;
inline constexpr uint32_t environment_cook_version = 1;
inline constexpr uint32_t imported_mesh_cook_version = 2; // 2: with joints and weights (#1038)
inline constexpr uint32_t imported_texture_cook_version = 1;
inline constexpr uint32_t skin_cook_version = 1;
inline constexpr uint32_t animation_cook_version = 1;

/// What an entry is cooked from: its kind and version, the source's digest, and the settings that change
/// the result, written as text (e.g. "role color\ncompression astc\nmips on\nastc on\n").
struct CookKey {
    std::string kind; // "texture", "environment", "mesh", or "imported-texture": the entry's extension
    uint32_t version = 0;
    Sha256Digest source{};
    std::string settings;
    /// The digest of all of it: the entry's name.
    Sha256Digest digest() const;
};

class CookCache {
public:
    /// `folder` need not exist; it is created, with a .gitignore of `*`, when the first entry is written.
    explicit CookCache(std::filesystem::path folder);
    const std::filesystem::path& folder() const noexcept { return m_folder; }

    /// The entry's bytes, or null when there is none or it is damaged (its stored digest does not match).
    std::optional<std::vector<std::byte>> read(const CookKey& key);
    /// Writes an entry atomically. A failure (e.g. a read-only project) is counted, never fatal: the
    /// asset was cooked and loads anyway.
    void write(const CookKey& key, std::span<const std::byte> payload);
    /// The digest of a file's bytes, remembered while the file's size and write time stay the same.
    Sha256Digest source_digest(const std::filesystem::path& file, std::span<const std::byte> bytes);
    /// The same, reading the file only when it changed; null when it cannot be read.
    std::optional<Sha256Digest> source_digest(const std::filesystem::path& file);

    struct Stats {
        size_t hits = 0, misses = 0, writes = 0, failures = 0, damaged = 0;
    };
    const Stats& stats() const noexcept { return m_stats; }
    std::filesystem::path entry_path(const CookKey& key) const;

    /// The entries on disk and their bytes, walking the folder.
    struct Usage {
        size_t entries = 0;
        uint64_t bytes = 0;
    };
    Usage usage() const;
    /// What pruning did (#1063, docs/assets.md#cook-cache).
    struct Pruning {
        size_t kept = 0, removed = 0;
        uint64_t kept_bytes = 0, removed_bytes = 0;
        std::vector<std::string> errors; // entries that could not be removed
    };
    /// Removes every entry whose key is not one of `reachable` (the keys loading could read now:
    /// reachable_cook_keys); files that are not entries are left alone. A dry run removes nothing and
    /// reports what it would.
    Pruning prune(std::span<const CookKey> reachable, bool dry_run = false);

private:
    struct Remembered {
        std::filesystem::file_time_type time;
        uintmax_t size = 0;
        Sha256Digest digest{};
    };
    std::filesystem::path m_folder;
    Stats m_stats;
    std::vector<std::pair<std::filesystem::path, Remembered>> m_sources;
};

/// A cooked payload in the checked envelope the cache's entries and a package's cooked files share:
/// "MAYACOOK", the format, the payload's size and digest, then the payload. Unwrapping gives the payload
/// back, or nothing when the bytes are damaged.
std::vector<std::byte> wrap_cooked(std::span<const std::byte> payload);
std::optional<std::vector<std::byte>> unwrap_cooked(std::span<const std::byte> bytes);

/// `<project folder>/.maya/cache`: the folder holding the project file.
std::filesystem::path cook_cache_folder(const Project& project);
} // namespace maya
