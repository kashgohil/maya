#pragma once

#include "maya/world/world.hpp"
#include <optional>
#include <unordered_map>
#include <utility>

namespace maya {

/// World poses (#1065: double translations) shown in place of the World's own for some entities: a play session's poses
/// between fixed ticks (PlaySession::presentation). Presentation only: nothing here is written back
/// to the World or to physics. Entities not listed show World::world_matrix.
class PresentationPoses {
public:
    void set(EntityHandle entity, const math::Affine& world) { m_poses[entity.slot] = {entity, world}; }
    std::optional<math::Affine> find(EntityHandle entity) const {
        const auto found = m_poses.find(entity.slot);
        if (found == m_poses.end() || found->second.first != entity) return std::nullopt;
        return found->second.second;
    }
    /// The world matrix to show for `entity`: this set's, else the World's.
    std::optional<math::Affine> world_matrix(const World& world, EntityHandle entity) const {
        if (auto pose = find(entity)) return pose;
        return world.world_matrix(entity);
    }
    void reserve(size_t count) { m_poses.reserve(count); }
    size_t size() const noexcept { return m_poses.size(); }
    bool empty() const noexcept { return m_poses.empty(); }

private:
    std::unordered_map<uint32_t, std::pair<EntityHandle, math::Affine>> m_poses; // by entity slot
};

} // namespace maya
