#pragma once

#include "maya/core/identity.hpp"

namespace maya {
class MeshAsset;
struct MaterialAsset;

/// A serializable reference, not a residency lease. AssetRegistry issues explicit residency leases.
template<class Asset>
struct AssetRef {
    AssetId id{};
    constexpr bool valid() const noexcept { return id.valid(); }
    auto operator<=>(const AssetRef&) const = default;
};
} // namespace maya
