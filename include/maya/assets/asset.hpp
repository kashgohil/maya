#pragma once
#include "maya/assets/asset_ref.hpp"
#include "maya/core/mesh.hpp"
#include <concepts>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace maya {
enum class AssetKind { mesh, material };
enum class AssetState { unloaded, loading, ready, failed };
enum class AssetError {
    none, invalid_id, duplicate_id, duplicate_path, invalid_path, not_registered,
    wrong_type, stale_handle, busy, missing_file, load_failed, invalid_data, device_unavailable
};
struct AssetDiagnostic {
    AssetError code = AssetError::none;
    std::string message;
    explicit operator bool() const noexcept { return code != AssetError::none; }
};

class MeshAsset {
public:
    explicit MeshAsset(std::unique_ptr<Mesh> mesh) : m_mesh(std::move(mesh)) {
        if (!m_mesh || !m_mesh->valid()) throw std::invalid_argument("MeshAsset requires a valid mesh");
    }
    const Mesh& mesh() const noexcept { return *m_mesh; }
private:
    std::unique_ptr<Mesh> m_mesh;
};
/// Initial material factors, interpreted by the future renderer; linear base color.
struct MaterialAsset {
    math::Vec4 base_color{1,1,1,1};
    float metallic = 0;
    float roughness = 1;
};
template<class T> concept Asset = std::same_as<T,MeshAsset> || std::same_as<T,MaterialAsset>;
template<Asset T> inline constexpr AssetKind asset_kind = std::same_as<T,MeshAsset> ? AssetKind::mesh : AssetKind::material;

template<Asset T> struct AssetHandle {
    uint64_t registry = 0;
    uint32_t slot = std::numeric_limits<uint32_t>::max();
    uint64_t generation = 0;
    auto operator<=>(const AssetHandle&) const = default;
};
/// A copyable residency lease, owning one immutable loaded version and its resources.
template<Asset T> class AssetLease {
public:
    AssetLease() = default;
    explicit operator bool() const noexcept { return static_cast<bool>(m_value); }
    const T& value() const {
        if (!m_value) throw std::logic_error("Empty asset lease");
        return *m_value;
    }
    AssetHandle<T> handle() const noexcept { return m_handle; }
    AssetRef<T> reference() const noexcept { return m_reference; }
private:
    friend class AssetRegistry;
    AssetLease(std::shared_ptr<const T> value, AssetHandle<T> handle, AssetRef<T> reference)
        : m_value(std::move(value)), m_handle(handle), m_reference(reference) {}
    std::shared_ptr<const T> m_value;
    AssetHandle<T> m_handle;
    AssetRef<T> m_reference;
};
template<Asset T> struct AssetResult {
    AssetLease<T> lease;
    AssetDiagnostic diagnostic;
    explicit operator bool() const noexcept { return static_cast<bool>(lease); }
};
template<Asset T> struct AssetLoadResult {
    std::shared_ptr<const T> value;
    AssetDiagnostic diagnostic;
};
struct AssetRecord {
    AssetId id;
    AssetKind kind;
    std::filesystem::path path; // normalized, project-relative, never a serialized runtime handle
};
struct AssetInfo {
    AssetRecord record;
    AssetState state = AssetState::unloaded;
    uint64_t generation = 0;
    AssetDiagnostic diagnostic; // failed reload may coexist with a ready previous version
};
class AssetProvider {
public:
    virtual ~AssetProvider() = default;
    virtual AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path& absolute_path) = 0;
    virtual AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path& absolute_path) = 0;
};
/// Initial adapter: existing OBJ loader and a small versioned material-factor file.
class FileAssetProvider final : public AssetProvider {
public:
    explicit FileAssetProvider(GraphicsDevice& device) : m_device(device), m_lifetime(device.resource_lifetime()) {}
    AssetLoadResult<MeshAsset> load_mesh(const std::filesystem::path& path) override;
    AssetLoadResult<MaterialAsset> load_material(const std::filesystem::path& path) override;
private:
    GraphicsDevice& m_device;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
};
/// Explicit fallback: missing meshes skip their draw; failed materials may use this value.
const MaterialAsset& fallback_material() noexcept;
} // namespace maya
