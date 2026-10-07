#pragma once
#include "maya/assets/asset.hpp"
#include <iosfwd>
#include <optional>
#include <unordered_map>
#include <variant>

namespace maya {
/// What a registry holds now. Bytes are tracked from buffer descriptors and CPU copies, not platform
/// residency.
struct AssetResidency {
    size_t entries = 0; // catalog entries
    size_t unloaded = 0, ready = 0, failed = 0; // entries by state
    size_t meshes = 0, materials = 0, scripts = 0, textures = 0, environments = 0, skins = 0, animations = 0; // resident versions owned by the cache
    size_t leased = 0; // resident versions also held by a lease outside the registry
    size_t mesh_gpu_bytes = 0; // vertex and index buffers of resident meshes
    size_t mesh_cpu_bytes = 0; // their picking geometry (MeshGeometry)
    size_t texture_gpu_bytes = 0; // every mip level of resident textures (texture_bytes)
    size_t environment_gpu_bytes = 0; // resident environments' backgrounds and specular cubes
};

/// Single-owner-thread registry. Providers execute synchronously at the caller's load boundary.
/// It owns one cache lease per ready entry; evict_unused releases entries with no external lease.
class AssetRegistry {
public:
    AssetRegistry(std::filesystem::path project_root, std::unique_ptr<AssetProvider> provider);
    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;
    AssetRegistry(AssetRegistry&&) = delete;
    AssetRegistry& operator=(AssetRegistry&&) = delete;

    AssetDiagnostic register_asset(AssetRecord record);
    template<Asset T> AssetDiagnostic register_asset(AssetRef<T> ref, std::filesystem::path path) {
        return register_asset({ref.id, asset_kind<T>, std::move(path)});
    }
    std::vector<AssetRecord> records() const;
    std::optional<AssetInfo> info(AssetId id) const;
    /// Counts and bytes of what is resident; O(catalog size).
    AssetResidency residency() const noexcept;
    uint64_t token() const noexcept { return m_token; }
    /// The project directory that catalog paths are relative to.
    const std::filesystem::path& root() const noexcept { return m_root; }

    template<Asset T> AssetResult<T> acquire(AssetRef<T> ref) { return load<T>(ref, false); }
    /// Failed reload preserves the previously published version and runtime generation.
    template<Asset T> AssetResult<T> reload(AssetRef<T> ref) { return load<T>(ref, true); }
    template<Asset T> AssetResult<T> resolve(AssetHandle<T> handle) const {
        if (handle.registry != m_token || handle.slot >= m_entries.size())
            return {{}, {AssetError::stale_handle, "Asset handle belongs to another registry or invalid slot"}};
        const auto& entry = m_entries[handle.slot];
        if (entry.record.kind != asset_kind<T>)
            return {{}, {AssetError::wrong_type, "Asset handle type does not match the catalog entry"}};
        if (entry.generation != handle.generation || !usable(entry.payload))
            return {{}, {AssetError::stale_handle, "Asset version was evicted, replaced, or its device session ended"}};
        return {AssetLease<T>{std::get<std::shared_ptr<const T>>(entry.payload),handle,{entry.record.id}}, {}};
    }
    size_t evict_unused();
    /// Publishes `value` as the material's next version, as a successful reload would, without reading
    /// its file: an editor's unsaved edit (docs/editor.md#materials). Later acquisitions and extractions
    /// see it; leases of the previous version keep it. Fails for an ID that is not a cataloged material.
    AssetDiagnostic publish(AssetRef<MaterialAsset> ref, MaterialAsset value);

private:
    using Payload = std::variant<std::shared_ptr<const MeshAsset>, std::shared_ptr<const MaterialAsset>,
                                 std::shared_ptr<const ScriptAsset>, std::shared_ptr<const TextureAsset>,
                                 std::shared_ptr<const EnvironmentAsset>, std::shared_ptr<const SkinAsset>,
                                 std::shared_ptr<const AnimationAsset>>;
    struct Entry {
        AssetRecord record;
        AssetState state = AssetState::unloaded;
        uint64_t generation = 0;
        Payload payload{};
        AssetDiagnostic diagnostic{};
    };
    struct LoadOutcome { uint32_t slot = 0; AssetDiagnostic diagnostic; };
    template<Asset T> AssetResult<T> load(AssetRef<T> ref, bool reload) {
        const auto result = load_entry(ref.id, asset_kind<T>, reload);
        if (result.diagnostic) return {{},result.diagnostic};
        const auto& entry = m_entries[result.slot];
        return {AssetLease<T>{std::get<std::shared_ptr<const T>>(entry.payload),
            {m_token,result.slot,entry.generation},ref}, {}};
    }
    static bool usable(const Payload& payload) noexcept;
    LoadOutcome load_entry(AssetId id, AssetKind kind, bool reload);
    std::optional<std::filesystem::path> resolve_path(const std::filesystem::path& path) const;
    const uint64_t m_token;
    const std::filesystem::path m_root;
    std::unique_ptr<AssetProvider> m_provider;
    bool m_loading = false;
    std::vector<Entry> m_entries;
    std::unordered_map<AssetId,uint32_t,PersistentIdHash> m_ids;
    std::unordered_map<std::string,AssetId> m_paths;
};

struct AssetCatalogResult {
    std::vector<AssetRecord> records;
    AssetDiagnostic diagnostic;
    explicit operator bool() const noexcept { return !diagnostic; }
};
/// Versioned catalog of IDs, types, and quoted project-relative source paths; no runtime state.
AssetCatalogResult read_asset_catalog(std::istream& input);
void write_asset_catalog(std::ostream& output, const std::vector<AssetRecord>& records);
} // namespace maya
