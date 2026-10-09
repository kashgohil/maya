#include "maya/renderer/visibility_index.hpp"
#include "maya/world/components.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

namespace maya {
namespace {
uint64_t key_of(double x, double z, double size) noexcept {
    const auto cx = int32_t(std::floor(x / size)), cz = int32_t(std::floor(z / size));
    return (uint64_t(uint32_t(cx)) << 32) | uint32_t(cz);
}
double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
math::Vec3 relative(const math::DVec3& p, const math::DVec3& origin) noexcept {
    return {float(p.x - origin.x), float(p.y - origin.y), float(p.z - origin.z)};
}
} // namespace

VisibilityIndex::VisibilityIndex(const World& world)
    : m_world(&world), m_lifetime(world.lifetime()), m_token(world.token()), m_log(world.open_change_log()) {}

VisibilityIndex::~VisibilityIndex() {
    if (!m_lifetime.expired()) m_world->close_change_log(m_log);
}

VisibilityIndex::Item& VisibilityIndex::item(EntityHandle entity) {
    if (entity.slot >= m_items.size()) m_items.resize(size_t(entity.slot) + 1);
    return m_items[entity.slot];
}

const VisibilityIndex::MeshBounds* VisibilityIndex::bounds(const AssetRegistry& assets, AssetId mesh) {
    if (const auto found = m_meshes.find(mesh); found != m_meshes.end()) return &found->second;
    const auto lease = assets.resident(AssetRef<MeshAsset>{mesh});
    if (!lease) return nullptr; // not resident: unbounded until it is
    auto bounds = MeshBounds{};
    bounds.generation = lease.handle().generation;
    if (const auto& geometry = lease.value().geometry(); !geometry.empty()) {
        bounds.center = (geometry.min + geometry.max) * 0.5f;
        bounds.half_diagonal = (geometry.max - geometry.min).length() * 0.5f;
        bounds.bounded = true;
    }
    bounds.skinned = lease.value().mesh().skinned();
    return &m_meshes.emplace(mesh, bounds).first->second;
}

void VisibilityIndex::unplace(Item& entry) {
    const auto remove = [&](std::vector<uint32_t>& list) {
        const auto last = list.back();
        list[entry.position] = last;
        m_items[last].position = entry.position;
        list.pop_back();
    };
    switch (entry.where) {
    case Where::grid:
    case Where::large: {
        auto& cells = entry.where == Where::grid ? m_grid : m_large;
        const auto found = cells.find(entry.cell);
        remove(found->second.slots);
        if (found->second.slots.empty()) cells.erase(found); // its bounds go with it
        --(entry.where == Where::grid ? m_stats.gridded : m_stats.large);
        break;
    }
    case Where::unbounded:
        remove(m_unbounded);
        --m_stats.unbounded;
        break;
    case Where::hidden:
        --m_stats.hidden;
        break;
    case Where::none:
        break;
    }
    entry.where = Where::none;
}

void VisibilityIndex::place(Item& entry, Where where, uint64_t cell) {
    const auto slot = entry.entity.slot;
    if (entry.where != where || ((where == Where::grid || where == Where::large) && entry.cell != cell)) {
        unplace(entry);
        entry.where = where;
        entry.cell = cell;
        switch (where) {
        case Where::grid:
        case Where::large: {
            auto& list = (where == Where::grid ? m_grid : m_large)[cell].slots;
            entry.position = uint32_t(list.size());
            list.push_back(slot);
            ++(where == Where::grid ? m_stats.gridded : m_stats.large);
            break;
        }
        case Where::unbounded:
            entry.position = uint32_t(m_unbounded.size());
            m_unbounded.push_back(slot);
            ++m_stats.unbounded;
            break;
        case Where::hidden:
            ++m_stats.hidden;
            break;
        case Where::none:
            break;
        }
    }
    if (where != Where::grid && where != Where::large) return;
    // A loose cell's bounds grow to hold every sphere put in it, and never shrink while it has any.
    auto& bounds = (where == Where::grid ? m_grid : m_large)[cell];
    const auto r = double(entry.radius);
    const auto low = math::DVec3{entry.center.x - r, entry.center.y - r, entry.center.z - r};
    const auto high = math::DVec3{entry.center.x + r, entry.center.y + r, entry.center.z + r};
    if (bounds.slots.size() == 1 && bounds.slots.front() == slot) {
        bounds.low = low;
        bounds.high = high;
    } else {
        bounds.low = {std::min(bounds.low.x, low.x), std::min(bounds.low.y, low.y), std::min(bounds.low.z, low.z)};
        bounds.high = {std::max(bounds.high.x, high.x), std::max(bounds.high.y, high.y), std::max(bounds.high.z, high.z)};
    }
}

void VisibilityIndex::evaluate(const World& world, const AssetRegistry& assets, EntityHandle entity) {
    ++m_stats.updated;
    auto& entry = item(entity);
    if (entry.where != Where::none && entry.entity != entity) {
        // Its slot holds another entity: a newer one, already indexed, or an older one, now gone.
        if (world.alive(entry.entity)) {
            if (!world.alive(entity)) return;
        }
        unplace(entry);
    }
    if (!world.alive(entity)) { // destroyed, or staged: no longer drawn
        if (entry.entity == entity) unplace(entry);
        return;
    }
    auto renderer = std::optional<MeshRendererComponent>{};
    world.with<MeshRendererComponent>(entity, [&](const MeshRendererComponent& component) { renderer = component; });
    if (!renderer) {
        unplace(entry);
        return;
    }
    entry.entity = entity;
    entry.renderer = *renderer;
    if (!renderer->visible || !renderer->mesh.valid()) return place(entry, Where::hidden, 0);
    entry.mesh = renderer->mesh.id;
    const auto pose = world.world_matrix(entity);
    const auto* mesh = pose ? bounds(assets, renderer->mesh.id) : nullptr;
    if (!pose || !mesh || !mesh->bounded || (mesh->skinned && world.has<SkinComponent>(entity))) return place(entry, Where::unbounded, 0);
    const auto& m = pose->linear;
    const auto& c = mesh->center;
    entry.center = {pose->translation.x + double(m.at(0, 0) * c.x + m.at(0, 1) * c.y + m.at(0, 2) * c.z),
                    pose->translation.y + double(m.at(1, 0) * c.x + m.at(1, 1) * c.y + m.at(1, 2) * c.z),
                    pose->translation.z + double(m.at(2, 0) * c.x + m.at(2, 1) * c.y + m.at(2, 2) * c.z)};
    const auto column = [&](int i) { return math::Vec3{m.at(0, i), m.at(1, i), m.at(2, i)}.length(); };
    entry.radius = mesh->half_diagonal * std::max({column(0), column(1), column(2)});
    if (!std::isfinite(entry.radius) || !std::isfinite(entry.center.x) || !std::isfinite(entry.center.y) || !std::isfinite(entry.center.z))
        return place(entry, Where::unbounded, 0);
    if (entry.radius <= slack) place(entry, Where::grid, key_of(entry.center.x, entry.center.z, cell_size));
    else place(entry, Where::large, key_of(entry.center.x, entry.center.z, large_cell_size));
}

void VisibilityIndex::update(const World& world, const AssetRegistry& assets) {
    if (world.token() != m_token) throw std::invalid_argument("VisibilityIndex::update with another World");
    const auto clock = std::chrono::steady_clock::now();
    m_stats.updated = 0;
    ++m_stamp;
    const auto visit = [&](EntityHandle entity) {
        if (entity.slot < m_items.size() && m_items[entity.slot].seen == m_stamp && m_items[entity.slot].entity == entity) return;
        evaluate(world, assets, entity);
        item(entity).seen = m_stamp;
    };
    auto changes = m_world->take_changes(m_log);
    if (!m_built || changes.all) { // the first update, or its log overflowed: every mesh renderer, now
        m_items.clear();
        m_grid.clear();
        m_large.clear();
        m_unbounded.clear();
        m_stats.gridded = m_stats.large = m_stats.unbounded = m_stats.hidden = 0;
        world.for_each<MeshRendererComponent>([&](EntityHandle entity, const MeshRendererComponent&) { visit(entity); });
        m_built = true;
        ++m_stats.rebuilds;
    } else {
        // Meshes reloaded since their bounds were read: their instances again.
        auto reloaded = std::vector<AssetId>{};
        for (const auto& [id, cached] : m_meshes)
            if (const auto info = assets.info(id); !info || info->generation != cached.generation) reloaded.push_back(id);
        for (const auto id : reloaded) m_meshes.erase(id);
        if (!reloaded.empty())
            for (const auto& entry : m_items)
                if (entry.where != Where::none && entry.where != Where::hidden && std::ranges::find(reloaded, entry.mesh) != reloaded.end())
                    m_work.push_back(entry.entity);
        // Instances waiting for their mesh to be resident.
        for (const auto slot : m_unbounded)
            if (!m_meshes.contains(m_items[slot].mesh)) m_work.push_back(m_items[slot].entity);
        for (const auto entity : m_work) visit(entity);
        m_work.clear();
        // What changed, and everything below what moved.
        auto& changed = changes.entities;
        while (!changed.empty()) {
            const auto entity = changed.back();
            changed.pop_back();
            visit(entity);
            // Each changed entity's children once, though an earlier pass may have visited it.
            if (auto& entry = item(entity); entry.expanded != m_stamp && world.alive(entity)) {
                entry.expanded = m_stamp;
                for (const auto child : world.children(entity)) changed.push_back(child);
            }
        }
    }
    m_stats.renderers = m_stats.gridded + m_stats.large + m_stats.unbounded + m_stats.hidden;
    m_stats.cells = m_grid.size();
    m_stats.large_cells = m_large.size();
    m_stats.update_ms = milliseconds_since(clock);
}

void VisibilityIndex::query(std::span<const Frustum> volumes, const math::DVec3& origin, std::vector<EntityHandle>& out) {
    const auto clock = std::chrono::steady_clock::now();
    ++m_stamp;
    ++m_frame;
    const auto before = out.size();
    // A bit per slot, read out in slot order at the end: each entity once, and in the order the World
    // stores them, so extraction reads its components in order rather than cell by cell.
    m_returned.assign((m_items.size() + 63) / 64, 0);
    const auto take = [&](uint32_t slot) { m_returned[slot >> 6] |= uint64_t{1} << (slot & 63); };
    const auto search = [&](const std::unordered_map<uint64_t, Cell>& cells) {
        for (const auto& [key, cell] : cells) {
            const auto low = relative(cell.low, origin), high = relative(cell.high, origin);
            auto best = Containment::outside;
            for (const auto& volume : volumes) {
                const auto where = volume.classify(low, high);
                if (where == Containment::inside) {
                    best = where;
                    break;
                }
                if (where == Containment::intersects) best = where;
            }
            if (best == Containment::outside) continue;
            for (const auto slot : cell.slots) {
                if (best == Containment::inside) {
                    take(slot);
                    continue;
                }
                const auto& entry = m_items[slot];
                const auto center = relative(entry.center, origin);
                if (std::ranges::any_of(volumes, [&](const Frustum& volume) { return volume.reaches(center, entry.radius); })) take(slot);
            }
        }
    };
    search(m_grid);
    search(m_large);
    for (const auto slot : m_unbounded) take(slot);
    for (size_t word = 0; word < m_returned.size(); ++word)
        for (auto bits = m_returned[word]; bits != 0; bits &= bits - 1)
            out.push_back(m_items[word * 64 + size_t(std::countr_zero(bits))].entity);
    m_queried = m_stamp;
    m_stats.returned = out.size() - before;
    m_stats.query_ms = milliseconds_since(clock);
}

bool VisibilityIndex::returned(EntityHandle entity) const noexcept {
    return m_queried == m_stamp && size_t(entity.slot >> 6) < m_returned.size() &&
           (m_returned[entity.slot >> 6] >> (entity.slot & 63) & 1) != 0 && m_items[entity.slot].entity == entity;
}

std::optional<uint8_t> VisibilityIndex::level(EntityHandle entity) const noexcept {
    if (entity.slot >= m_items.size()) return std::nullopt;
    const auto& entry = m_items[entity.slot];
    if (entry.entity != entity || entry.drawn + 1 != m_frame) return std::nullopt;
    return entry.level;
}

void VisibilityIndex::set_level(EntityHandle entity, uint8_t level) noexcept {
    if (entity.slot >= m_items.size() || m_items[entity.slot].entity != entity) return;
    m_items[entity.slot].level = level;
    m_items[entity.slot].drawn = m_frame;
}

} // namespace maya
