#pragma once

#include "maya/core/identity.hpp"

namespace maya {
struct MeshAsset;
struct MaterialAsset;

/// A serializable reference, not a residency lease. Registry/loading comes in #993.
template<class Asset>
struct AssetRef {
    AssetId id{};
    constexpr bool valid() const noexcept { return id.valid(); }
    auto operator<=>(const AssetRef&) const = default;
};
} // namespace maya
