#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace maya {
namespace detail {
std::array<uint64_t, 2> generate_persistent_id();
uint64_t next_lifetime_token();
}

/// Persistent value only: never a storage address or a runtime handle.
template<class Tag>
struct PersistentId {
    uint64_t high = 0;
    uint64_t low = 0;

    constexpr bool valid() const noexcept { return high != 0 || low != 0; }
    auto operator<=>(const PersistentId&) const = default;
    static PersistentId generate() {
        const auto words = detail::generate_persistent_id();
        return {words[0], words[1]};
    }
};

struct EntityIdTag;
struct AssetIdTag;
using EntityId = PersistentId<EntityIdTag>;
using AssetId = PersistentId<AssetIdTag>;

struct PersistentIdHash {
    template<class Tag>
    size_t operator()(PersistentId<Tag> id) const noexcept {
        const auto first = std::hash<uint64_t>{}(id.high);
        const auto second = std::hash<uint64_t>{}(id.low);
        return first ^ (second + 0x9e3779b9u + (first << 6) + (first >> 2));
    }
};
} // namespace maya
