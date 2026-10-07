// Visibility prototype for the world-scale decisions (#1060, docs/architecture/world-scale-decision.md).
//
// 1. Spatial index: 100,000 boxes scattered over 2 km x 2 km (W1-like scatter), a tenth of them moving a
//    little every tick. Candidates: brute force (what extraction and culling do today), a loose grid of
//    32 m cells, a dynamic bounding-volume tree with fattened boxes (as Box2D and Jolt's broad phase), and a
//    loose octree. Each is built, updated for 60 ticks, and queried each frame for the camera (far planes
//    of 1,500 m and 250 m), four shadow cascades, and a spot light. Every query must return exactly what
//    brute force returns.
// 2. LOD: meshoptimizer simplifies R1's models (ABeautifulGame, FlightHelmet, DamagedHelmet) to 1/2 ... 1/16
//    of their triangles, with positions only and with normals and texture coordinates; time, triangles
//    kept, error, and the distance from which each level is under a pixel on a 1080p, 60 degree view.
//    Without the samples (tools/fetch_render_samples.sh) part 2 is skipped.
// Lines starting UNEXPECTED report what the decision relies on and make the run fail.

#include <cgltf.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
int failures = 0;
void unexpected(const std::string& what) {
    std::printf("UNEXPECTED: %s\n", what.c_str());
    ++failures;
}
uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
float unit(uint64_t seed) { return float(mix(seed) >> 40) / float(1u << 24); }

// --- Geometry ------------------------------------------------------------------------------------------
struct V3 {
    float x, y, z;
    V3 operator+(V3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(V3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
};
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalize(V3 v) { return v * (1.0f / std::sqrt(dot(v, v))); }
V3 vmin(V3 a, V3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
V3 vmax(V3 a, V3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
struct Box {
    V3 lo, hi;
    bool contains(const Box& b) const { return lo.x <= b.lo.x && lo.y <= b.lo.y && lo.z <= b.lo.z && hi.x >= b.hi.x && hi.y >= b.hi.y && hi.z >= b.hi.z; }
    float area() const {
        const auto d = hi - lo;
        return 2 * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
};
Box merge(const Box& a, const Box& b) { return {vmin(a.lo, b.lo), vmax(a.hi, b.hi)}; }

// A convex volume as inward-facing planes (n.p + d >= 0 inside).
struct Plane {
    V3 n;
    float d;
};
struct Volume {
    std::vector<Plane> planes;
};
enum class Overlap { outside, intersects, inside };
Overlap classify(const Volume& volume, const Box& box) {
    auto result = Overlap::inside;
    for (const auto& p : volume.planes) {
        const auto positive = V3{p.n.x >= 0 ? box.hi.x : box.lo.x, p.n.y >= 0 ? box.hi.y : box.lo.y, p.n.z >= 0 ? box.hi.z : box.lo.z};
        const auto negative = V3{p.n.x >= 0 ? box.lo.x : box.hi.x, p.n.y >= 0 ? box.lo.y : box.hi.y, p.n.z >= 0 ? box.lo.z : box.hi.z};
        if (dot(p.n, positive) + p.d < 0) return Overlap::outside;
        if (dot(p.n, negative) + p.d < 0) result = Overlap::intersects;
    }
    return result;
}
bool visible(const Volume& volume, const Box& box) { return classify(volume, box) != Overlap::outside; }

Plane plane_through(V3 point, V3 inward) {
    const auto n = normalize(inward);
    return {n, -dot(n, point)};
}
// A perspective frustum from an eye, a forward direction, a vertical field of view, and near/far distances.
Volume frustum(V3 eye, V3 forward, float fov_y, float aspect, float near, float far) {
    const auto f = normalize(forward), right = normalize(cross(f, {0, 1, 0})), up = cross(right, f);
    const auto ty = std::tan(fov_y * 0.5f), tx = ty * aspect;
    auto v = Volume{};
    v.planes.push_back(plane_through(eye + f * near, f));
    v.planes.push_back(plane_through(eye + f * far, f * -1));
    v.planes.push_back({normalize(cross(up, f + right * tx)), 0}); // right: inward normal
    v.planes.push_back({normalize(cross(f - right * tx, up)), 0});  // left
    v.planes.push_back({normalize(cross(f + up * ty, right)), 0});  // top
    v.planes.push_back({normalize(cross(right, f - up * ty)), 0});  // bottom
    for (size_t i = 2; i < 6; ++i) v.planes[i].d = -dot(v.planes[i].n, eye);
    return v;
}
// An axis-aligned box as a volume.
Volume box_volume(V3 lo, V3 hi) {
    auto v = Volume{};
    v.planes = {{{1, 0, 0}, -lo.x}, {{-1, 0, 0}, hi.x}, {{0, 1, 0}, -lo.y}, {{0, -1, 0}, hi.y}, {{0, 0, 1}, -lo.z}, {{0, 0, -1}, hi.z}};
    return v;
}
// An orthographic box along a light direction around a centre: a shadow cascade's volume.
Volume cascade(V3 centre, V3 light, float half_width, float depth) {
    const auto l = normalize(light), a = normalize(cross(l, {0, 0, 1})), b = cross(a, l);
    auto v = Volume{};
    for (const auto& axis : {a, b}) {
        v.planes.push_back(plane_through(centre - axis * half_width, axis));
        v.planes.push_back(plane_through(centre + axis * half_width, axis * -1));
    }
    v.planes.push_back(plane_through(centre - l * depth, l));
    v.planes.push_back(plane_through(centre + l * half_width, l * -1));
    return v;
}

// --- Candidates ----------------------------------------------------------------------------------------
using Ids = std::vector<uint32_t>;
struct Index {
    virtual ~Index() = default;
    virtual const char* name() const = 0;
    virtual void insert(uint32_t id, const Box& box) = 0;
    virtual void move(uint32_t id, const Box& box) = 0;
    virtual void query(const Volume& volume, Ids& out) const = 0;
};

class Brute final : public Index {
public:
    const char* name() const override { return "brute force"; }
    void insert(uint32_t id, const Box& box) override {
        if (id >= m_boxes.size()) m_boxes.resize(id + 1);
        m_boxes[id] = box;
    }
    void move(uint32_t id, const Box& box) override { m_boxes[id] = box; }
    void query(const Volume& volume, Ids& out) const override {
        for (uint32_t i = 0; i < m_boxes.size(); ++i)
            if (visible(volume, m_boxes[i])) out.push_back(i);
    }

private:
    std::vector<Box> m_boxes;
};

// Objects live in the cell holding their centre; a cell's bounds grow by the largest object half size.
class LooseGrid final : public Index {
public:
    LooseGrid(float extent, float cell, float max_half) : m_cell(cell), m_side(int(std::ceil(extent / cell))), m_slack(max_half) {
        m_cells.resize(size_t(m_side * m_side));
    }
    const char* name() const override { return "loose grid 32m"; }
    void insert(uint32_t id, const Box& box) override {
        if (id >= m_entries.size()) m_entries.resize(id + 1);
        const auto c = cell_of(box);
        m_entries[id] = {box, c, uint32_t(m_cells[c].ids.size())};
        m_cells[c].ids.push_back(id);
        widen(c, box);
    }
    void move(uint32_t id, const Box& box) override {
        auto& entry = m_entries[id];
        const auto c = cell_of(box);
        if (c != entry.cell) {
            auto& from = m_cells[entry.cell].ids;
            m_entries[from.back()].slot = entry.slot;
            from[entry.slot] = from.back();
            from.pop_back();
            entry.cell = c;
            entry.slot = uint32_t(m_cells[c].ids.size());
            m_cells[c].ids.push_back(id);
        }
        entry.box = box;
        widen(c, box);
    }
    void query(const Volume& volume, Ids& out) const override {
        for (const auto& cell : m_cells) {
            if (cell.ids.empty()) continue;
            switch (classify(volume, cell.bounds)) {
            case Overlap::outside: break;
            case Overlap::inside: out.insert(out.end(), cell.ids.begin(), cell.ids.end()); break;
            case Overlap::intersects:
                for (const auto id : cell.ids)
                    if (visible(volume, m_entries[id].box)) out.push_back(id);
            }
        }
    }

private:
    struct Cell {
        Ids ids;
        Box bounds{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    };
    struct Entry {
        Box box;
        uint32_t cell, slot;
    };
    uint32_t cell_of(const Box& box) const {
        const auto centre = (box.lo + box.hi) * 0.5f;
        const auto x = std::clamp(int(centre.x / m_cell), 0, m_side - 1), z = std::clamp(int(centre.z / m_cell), 0, m_side - 1);
        return uint32_t(z * m_side + x);
    }
    // Cell bounds: the cell's footprint grown by the slack, and its height range as objects arrive.
    void widen(uint32_t c, const Box& box) {
        auto& bounds = m_cells[c].bounds;
        const auto x = float(c % uint32_t(m_side)) * m_cell, z = float(c / uint32_t(m_side)) * m_cell;
        bounds.lo = vmin(bounds.lo, {x - m_slack, box.lo.y, z - m_slack});
        bounds.hi = vmax(bounds.hi, {x + m_cell + m_slack, box.hi.y, z + m_cell + m_slack});
    }
    float m_cell;
    int m_side;
    float m_slack;
    std::vector<Cell> m_cells;
    std::vector<Entry> m_entries;
};

// A dynamic AABB tree: leaves hold fattened boxes, so small moves change nothing; inserts descend by
// surface-area cost, and the tree is rebalanced by rotations (after Box2D's b2DynamicTree).
class DynamicTree final : public Index {
public:
    explicit DynamicTree(float margin) : m_margin(margin) {}
    const char* name() const override { return "dynamic tree"; }
    void insert(uint32_t id, const Box& box) override {
        if (id >= m_leaf.size()) m_leaf.resize(id + 1, -1);
        m_leaf[id] = add_leaf(id, fat(box));
        m_tight.resize(m_leaf.size());
        m_tight[id] = box;
    }
    void move(uint32_t id, const Box& box) override {
        m_tight[id] = box;
        if (m_nodes[size_t(m_leaf[id])].box.contains(box)) return;
        remove_leaf(m_leaf[id]);
        m_leaf[id] = add_leaf(id, fat(box));
    }
    void query(const Volume& volume, Ids& out) const override {
        if (m_root < 0) return;
        auto stack = std::array<int, 128>{};
        auto top = 0;
        stack[top++] = m_root;
        while (top) {
            const auto& node = m_nodes[size_t(stack[--top])];
            const auto overlap = classify(volume, node.box);
            if (overlap == Overlap::outside) continue;
            if (node.leaf()) {
                if (overlap == Overlap::inside || visible(volume, m_tight[node.id])) out.push_back(node.id);
            } else if (overlap == Overlap::inside) {
                collect(int(&node - m_nodes.data()), out);
            } else {
                stack[top++] = node.child[0];
                stack[top++] = node.child[1];
            }
        }
    }

private:
    struct Node {
        Box box;
        int parent = -1, child[2] = {-1, -1}, height = 0;
        uint32_t id = 0;
        bool leaf() const { return child[0] < 0; }
    };
    Box fat(const Box& box) const { return {box.lo - V3{m_margin, m_margin, m_margin}, box.hi + V3{m_margin, m_margin, m_margin}}; }
    // Collects a subtree whose box is inside the volume; leaves still test their tight box, since the
    // fattened box may be inside where the object's own box is not (never the reverse), so nothing is lost.
    void collect(int index, Ids& out) const {
        const auto& node = m_nodes[size_t(index)];
        if (node.leaf()) {
            out.push_back(node.id);
            return;
        }
        collect(node.child[0], out);
        collect(node.child[1], out);
    }
    int allocate() {
        if (!m_free.empty()) {
            const auto i = m_free.back();
            m_free.pop_back();
            m_nodes[size_t(i)] = Node{};
            return i;
        }
        m_nodes.push_back({});
        return int(m_nodes.size() - 1);
    }
    int add_leaf(uint32_t id, const Box& box) {
        const auto leaf = allocate();
        m_nodes[size_t(leaf)].box = box;
        m_nodes[size_t(leaf)].id = id;
        if (m_root < 0) {
            m_root = leaf;
            return leaf;
        }
        auto index = m_root;
        while (!m_nodes[size_t(index)].leaf()) {
            const auto& node = m_nodes[size_t(index)];
            const auto area = node.box.area(), combined = merge(node.box, box).area();
            const auto cost = 2 * combined, inherit = 2 * (combined - area);
            auto child_cost = [&](int c) {
                const auto& child = m_nodes[size_t(c)];
                const auto merged = merge(child.box, box).area();
                return (child.leaf() ? merged : merged - child.box.area()) + inherit;
            };
            const auto c0 = child_cost(node.child[0]), c1 = child_cost(node.child[1]);
            if (cost < c0 && cost < c1) break;
            index = c0 < c1 ? node.child[0] : node.child[1];
        }
        const auto sibling = index, old_parent = m_nodes[size_t(sibling)].parent, parent = allocate();
        m_nodes[size_t(parent)].parent = old_parent;
        m_nodes[size_t(parent)].box = merge(box, m_nodes[size_t(sibling)].box);
        m_nodes[size_t(parent)].height = m_nodes[size_t(sibling)].height + 1;
        m_nodes[size_t(parent)].child[0] = sibling;
        m_nodes[size_t(parent)].child[1] = leaf;
        m_nodes[size_t(sibling)].parent = parent;
        m_nodes[size_t(leaf)].parent = parent;
        if (old_parent < 0) m_root = parent;
        else m_nodes[size_t(old_parent)].child[m_nodes[size_t(old_parent)].child[0] == sibling ? 0 : 1] = parent;
        refit(parent);
        return leaf;
    }
    void remove_leaf(int leaf) {
        if (leaf == m_root) {
            m_root = -1;
            m_free.push_back(leaf);
            return;
        }
        const auto parent = m_nodes[size_t(leaf)].parent, grand = m_nodes[size_t(parent)].parent;
        const auto sibling = m_nodes[size_t(parent)].child[m_nodes[size_t(parent)].child[0] == leaf ? 1 : 0];
        if (grand < 0) {
            m_root = sibling;
            m_nodes[size_t(sibling)].parent = -1;
        } else {
            m_nodes[size_t(grand)].child[m_nodes[size_t(grand)].child[0] == parent ? 0 : 1] = sibling;
            m_nodes[size_t(sibling)].parent = grand;
            refit(grand);
        }
        m_free.push_back(parent);
        m_free.push_back(leaf);
    }
    void refit(int index) {
        while (index >= 0) {
            index = balance(index);
            auto& node = m_nodes[size_t(index)];
            const auto& a = m_nodes[size_t(node.child[0])];
            const auto& b = m_nodes[size_t(node.child[1])];
            node.box = merge(a.box, b.box);
            node.height = 1 + std::max(a.height, b.height);
            index = node.parent;
        }
    }
    // One AVL-style rotation when a node's children differ in height by more than one.
    int balance(int a) {
        auto& A = m_nodes[size_t(a)];
        if (A.leaf() || A.height < 2) return a;
        const auto b = A.child[0], c = A.child[1];
        const auto diff = m_nodes[size_t(c)].height - m_nodes[size_t(b)].height;
        if (diff > 1) return rotate(a, c, 1);
        if (diff < -1) return rotate(a, b, 0);
        return a;
    }
    // Lifts `up` (A's child on side `side`) above A.
    int rotate(int a, int up, int side) {
        auto& A = m_nodes[size_t(a)];
        auto& U = m_nodes[size_t(up)];
        const auto f = U.child[0], g = U.child[1];
        U.child[0] = a;
        U.parent = A.parent;
        A.parent = up;
        if (U.parent >= 0) m_nodes[size_t(U.parent)].child[m_nodes[size_t(U.parent)].child[0] == a ? 0 : 1] = up;
        else m_root = up;
        const auto keep_f = m_nodes[size_t(f)].height > m_nodes[size_t(g)].height;
        const auto kept = keep_f ? f : g, given = keep_f ? g : f;
        U.child[1] = kept;
        A.child[side] = given;
        m_nodes[size_t(given)].parent = a;
        const auto& other = m_nodes[size_t(A.child[1 - side])];
        A.box = merge(other.box, m_nodes[size_t(given)].box);
        A.height = 1 + std::max(other.height, m_nodes[size_t(given)].height);
        U.box = merge(A.box, m_nodes[size_t(kept)].box);
        U.height = 1 + std::max(A.height, m_nodes[size_t(kept)].height);
        return up;
    }
    float m_margin;
    std::vector<Node> m_nodes;
    std::vector<int> m_free, m_leaf;
    std::vector<Box> m_tight;
    int m_root = -1;
};

// A loose octree: an object lives in the deepest node at least twice its size that holds its centre;
// node bounds are doubled, so they always hold their objects.
class LooseOctree final : public Index {
public:
    LooseOctree(V3 origin, float size, int depth) : m_origin(origin), m_size(size), m_depth(depth) { m_nodes.emplace_back(); }
    const char* name() const override { return "loose octree"; }
    void insert(uint32_t id, const Box& box) override {
        if (id >= m_entries.size()) m_entries.resize(id + 1);
        place(id, box);
    }
    void move(uint32_t id, const Box& box) override {
        auto& entry = m_entries[id];
        const auto node = node_for(box);
        if (node == entry.node) {
            entry.box = box;
            return;
        }
        auto& from = m_nodes[size_t(entry.node)].ids;
        m_entries[from.back()].slot = entry.slot;
        from[entry.slot] = from.back();
        from.pop_back();
        place(id, box);
    }
    void query(const Volume& volume, Ids& out) const override { visit(0, m_origin, m_size, volume, out, false); }

private:
    struct Node {
        Ids ids;
        std::array<int, 8> child{-1, -1, -1, -1, -1, -1, -1, -1};
    };
    struct Entry {
        Box box;
        int node;
        uint32_t slot;
    };
    int node_for(const Box& box) {
        const auto extent = box.hi - box.lo, centre = (box.lo + box.hi) * 0.5f;
        const auto object = std::max({extent.x, extent.y, extent.z});
        auto index = 0;
        auto origin = m_origin;
        auto size = m_size;
        for (int level = 0; level < m_depth && size * 0.5f >= 2 * object; ++level) {
            size *= 0.5f;
            const auto ix = centre.x >= origin.x + size, iy = centre.y >= origin.y + size, iz = centre.z >= origin.z + size;
            const auto octant = int(ix) | int(iy) << 1 | int(iz) << 2;
            origin = origin + V3{ix ? size : 0, iy ? size : 0, iz ? size : 0};
            auto next = m_nodes[size_t(index)].child[size_t(octant)];
            if (next < 0) {
                next = int(m_nodes.size());
                m_nodes[size_t(index)].child[size_t(octant)] = next;
                m_nodes.emplace_back();
            }
            index = next;
        }
        return index;
    }
    void place(uint32_t id, const Box& box) {
        const auto node = node_for(box);
        m_entries[id] = {box, node, uint32_t(m_nodes[size_t(node)].ids.size())};
        m_nodes[size_t(node)].ids.push_back(id);
    }
    void visit(int index, V3 origin, float size, const Volume& volume, Ids& out, bool inside) const {
        const auto& node = m_nodes[size_t(index)];
        if (!inside) {
            const auto half = size * 0.5f;
            const auto loose = Box{origin - V3{half, half, half}, origin + V3{size + half, size + half, size + half}};
            const auto overlap = classify(volume, loose);
            if (overlap == Overlap::outside) return;
            inside = overlap == Overlap::inside;
        }
        for (const auto id : node.ids)
            if (inside || visible(volume, m_entries[id].box)) out.push_back(id);
        const auto half = size * 0.5f;
        for (int octant = 0; octant < 8; ++octant)
            if (node.child[size_t(octant)] >= 0)
                visit(node.child[size_t(octant)], origin + V3{octant & 1 ? half : 0, octant & 2 ? half : 0, octant & 4 ? half : 0}, half, volume, out, inside);
    }
    V3 m_origin;
    float m_size;
    int m_depth;
    std::vector<Node> m_nodes;
    std::vector<Entry> m_entries;
};

// --- The workload --------------------------------------------------------------------------------------
constexpr uint32_t object_count = 100000;
constexpr float world_size = 2048;
Box object_box(uint32_t i, V3 offset = {0, 0, 0}) {
    const auto centre = V3{unit(i) * world_size, 0.5f, unit(i ^ 0xabcdefull) * world_size} + offset;
    const auto half = 0.4f + 0.2f * unit(i ^ 0x1234ull);
    return {centre - V3{half, half, half}, centre + V3{half, half, half}};
}
struct Views {
    std::vector<Volume> volumes;
    std::vector<const char*> names;
};
Views frame_views() {
    const auto eye = V3{1024, 1.7f, 1024}, forward = normalize(V3{1, -0.05f, 0.3f}), light = V3{-0.5f, -0.8f, -0.3f};
    auto views = Views{};
    views.volumes.push_back(frustum(eye, forward, 1.0472f, 16.0f / 9.0f, 0.1f, 1500));
    views.names.push_back("camera 1500 m");
    views.volumes.push_back(frustum(eye, forward, 1.0472f, 16.0f / 9.0f, 0.1f, 250));
    views.names.push_back("camera 250 m");
    for (const auto [reach, width] : {std::pair{10.0f, 12.0f}, {35.0f, 40.0f}, {110.0f, 120.0f}, {350.0f, 380.0f}}) {
        views.volumes.push_back(cascade(eye + forward * reach, light, width, 600));
        views.names.push_back("cascade");
    }
    views.volumes.push_back(frustum(eye + V3{5, 8, 3}, V3{0.2f, -1, 0.1f}, 0.785f, 1, 0.1f, 30));
    views.names.push_back("spot 30 m");
    return views;
}

struct IndexResult {
    std::string name;
    double build_ms = 0, update_ms = 0, frame_ms = 0;
    std::vector<double> view_ms;
    std::vector<uint64_t> counts; // a fingerprint of each view's sorted objects
};
IndexResult run(Index& index) {
    auto result = IndexResult{};
    result.name = index.name();
    auto start = Clock::now();
    for (uint32_t i = 0; i < object_count; ++i) index.insert(i, object_box(i));
    result.build_ms = ms_since(start);
    // 60 ticks: a seeded tenth of the objects walk up to 5 cm a tick.
    auto offsets = std::vector<V3>(object_count, V3{0, 0, 0});
    auto update = 0.0;
    for (uint32_t tick = 0; tick < 60; ++tick) {
        start = Clock::now();
        for (uint32_t k = 0; k < object_count / 10; ++k) {
            const auto i = uint32_t(mix(k) % object_count);
            offsets[i] = offsets[i] + V3{unit(i * 31 + tick) * 0.1f - 0.05f, 0, unit(i * 17 + tick) * 0.1f - 0.05f};
            index.move(i, object_box(i, offsets[i]));
        }
        update += ms_since(start);
    }
    result.update_ms = update / 60;
    const auto views = frame_views();
    auto ids = Ids{};
    ids.reserve(object_count);
    result.view_ms.assign(views.volumes.size(), 1e30);
    for (int repeat = 0; repeat < 5; ++repeat) {
        result.counts.clear();
        for (size_t v = 0; v < views.volumes.size(); ++v) {
            ids.clear();
            start = Clock::now();
            index.query(views.volumes[v], ids);
            result.view_ms[v] = std::min(result.view_ms[v], ms_since(start));
            std::sort(ids.begin(), ids.end());
            auto fingerprint = uint64_t(ids.size());
            for (const auto id : ids) fingerprint = mix(fingerprint ^ id);
            result.counts.push_back(fingerprint);
        }
    }
    result.frame_ms = 0;
    for (const auto ms : result.view_ms) result.frame_ms += ms;
    return result;
}

// --- LOD -----------------------------------------------------------------------------------------------
struct Mesh {
    std::vector<float> vertices; // position, normal, uv: 8 floats
    std::vector<unsigned> indices;
};
bool load(const std::filesystem::path& path, Mesh& mesh) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }
    for (size_t m = 0; m < data->meshes_count; ++m) {
        for (size_t p = 0; p < data->meshes[m].primitives_count; ++p) {
            const auto& primitive = data->meshes[m].primitives[p];
            if (primitive.type != cgltf_primitive_type_triangles || !primitive.indices) continue;
            const cgltf_accessor *position = nullptr, *normal = nullptr, *uv = nullptr;
            for (size_t a = 0; a < primitive.attributes_count; ++a) {
                const auto& attribute = primitive.attributes[a];
                if (attribute.type == cgltf_attribute_type_position) position = attribute.data;
                if (attribute.type == cgltf_attribute_type_normal) normal = attribute.data;
                if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) uv = attribute.data;
            }
            if (!position) continue;
            const auto base = unsigned(mesh.vertices.size() / 8);
            auto scratch = std::vector<float>(position->count * 3);
            cgltf_accessor_unpack_floats(position, scratch.data(), scratch.size());
            auto normals = std::vector<float>(position->count * 3, 0.0f), uvs = std::vector<float>(position->count * 2, 0.0f);
            if (normal) cgltf_accessor_unpack_floats(normal, normals.data(), normals.size());
            if (uv) cgltf_accessor_unpack_floats(uv, uvs.data(), uvs.size());
            for (size_t v = 0; v < position->count; ++v) {
                mesh.vertices.insert(mesh.vertices.end(), {scratch[v * 3], scratch[v * 3 + 1], scratch[v * 3 + 2], normals[v * 3],
                    normals[v * 3 + 1], normals[v * 3 + 2], uvs[v * 2], uvs[v * 2 + 1]});
            }
            for (size_t i = 0; i < primitive.indices->count; ++i) mesh.indices.push_back(base + unsigned(cgltf_accessor_read_index(primitive.indices, i)));
        }
    }
    cgltf_free(data);
    return !mesh.indices.empty();
}

void lod_report(const std::vector<std::pair<std::string, std::filesystem::path>>& models) {
    std::printf("\n2. LOD with meshoptimizer %d.%d: triangles kept, error (relative to the mesh's size), time;\n", MESHOPTIMIZER_VERSION / 1000,
        MESHOPTIMIZER_VERSION % 1000 / 10);
    std::printf("   'pixel from' is the distance (in units of the mesh's size) beyond which the error is under one pixel at 1080p, 60 degrees\n");
    for (const auto& [name_string, path] : models) {
        const auto* name = name_string.c_str();
        auto mesh = Mesh{};
        if (!load(path, mesh)) {
            std::printf("   %s: not loaded\n", name);
            continue;
        }
        const auto vertex_count = mesh.vertices.size() / 8;
        const auto scale = meshopt_simplifyScale(mesh.vertices.data(), vertex_count, sizeof(float) * 8);
        std::printf("   %s: %zu triangles, %zu vertices, %.2f m across\n", name, mesh.indices.size() / 3, vertex_count, double(scale));
        const float weights[5] = {0.5f, 0.5f, 0.5f, 1.0f, 1.0f};
        // Positions only; with normals and texture coordinates; and the same allowed to collapse across
        // attribute seams (permissive) and to drop components that become too small to see (prune).
        enum class Mode { position, attributes, permissive };
        for (const auto mode : {Mode::position, Mode::attributes, Mode::permissive}) {
            const auto label = mode == Mode::position ? "position  " : mode == Mode::attributes ? "attributes" : "permissive";
            auto source = mesh.indices;
            auto previous_error = 0.0f;
            for (int level = 1; level <= 4; ++level) {
                auto lod = std::vector<unsigned>(source.size());
                const auto target = size_t(double(mesh.indices.size()) / double(1 << level)) / 3 * 3;
                auto error = 0.0f;
                const auto start = Clock::now();
                const auto kept = mode == Mode::position
                    ? meshopt_simplify(lod.data(), source.data(), source.size(), mesh.vertices.data(), vertex_count, sizeof(float) * 8, target, 1.0f, 0, &error)
                    : meshopt_simplifyWithAttributes(lod.data(), source.data(), source.size(), mesh.vertices.data(), vertex_count, sizeof(float) * 8,
                          mesh.vertices.data() + 3, sizeof(float) * 8, weights, 5, nullptr, target, 1.0f,
                          mode == Mode::permissive ? meshopt_SimplifyPermissive | meshopt_SimplifyPrune : 0, &error);
                const auto ms = ms_since(start);
                lod.resize(kept);
                // Each level simplifies the previous one, so errors add up.
                error += previous_error;
                previous_error = error;
                const auto pixel_from = double(error) / (2.0 * std::tan(0.5236) / 1080.0);
                std::printf("     %s LOD%d: %6.1f%% of triangles, error %.4f, %7.1f ms, pixel from %6.0f sizes\n", label, level,
                    100.0 * double(kept) / double(mesh.indices.size()), double(error), ms, pixel_from);
                if (mode == Mode::permissive && kept > target * 3 / 2 + 3)
                    unexpected(std::string(name) + " LOD" + std::to_string(level) + " kept " + std::to_string(kept / 3) + " triangles for a target of " + std::to_string(target / 3));
                source = std::move(lod);
            }
        }
    }
}
} // namespace

int main() {
    std::printf("Visibility prototype (#1060)\n\n1. Spatial index: %u boxes over %.0f m x %.0f m, a tenth moving each tick\n", object_count, double(world_size), double(world_size));
    auto brute = Brute{};
    auto grid = LooseGrid(world_size, 32, 0.7f);
    auto tree = DynamicTree(0.2f);
    auto octree = LooseOctree({0, -1024, 0}, world_size, 10);
    auto results = std::vector<IndexResult>{};
    for (Index* index : std::initializer_list<Index*>{&brute, &grid, &tree, &octree}) results.push_back(run(*index));
    const auto views = frame_views();
    std::printf("%16s %9s %10s", "candidate", "build ms", "update ms");
    for (const auto* name : views.names) std::printf(" %13s", name);
    std::printf(" %9s\n", "frame ms");
    for (const auto& r : results) {
        std::printf("%16s %9.2f %10.3f", r.name.c_str(), r.build_ms, r.update_ms);
        for (const auto ms : r.view_ms) std::printf(" %13.3f", ms);
        std::printf(" %9.3f\n", r.frame_ms);
        if (r.counts != results.front().counts) unexpected(r.name + " returned different objects from brute force");
    }
    // Each view's object count, from brute force.
    std::printf("%16s %9s %10s", "visible", "", "");
    {
        auto ids = Ids{};
        for (const auto& volume : views.volumes) {
            ids.clear();
            brute.query(volume, ids);
            std::printf(" %13zu", ids.size());
        }
        std::printf("\n");
    }
    // Correctness beyond the seven timed views: 300 small random volumes (30 m spot lights anywhere, any
    // direction), where a cell or node boundary is likely to cut an object.
    for (Index* index : std::initializer_list<Index*>{&grid, &tree, &octree}) {
        auto expected = Ids{}, actual = Ids{};
        auto mismatches = 0;
        for (uint64_t v = 0; v < 300; ++v) {
            const auto eye = V3{unit(v * 3) * world_size, 2 + unit(v * 3 + 1) * 20, unit(v * 3 + 2) * world_size};
            const auto direction = V3{unit(v * 7) - 0.5f, -0.3f - unit(v * 7 + 1), unit(v * 7 + 2) - 0.5f};
            const auto volume = frustum(eye, direction, 0.785f, 1, 0.1f, 30);
            expected.clear();
            actual.clear();
            brute.query(volume, expected);
            index->query(volume, actual);
            std::sort(actual.begin(), actual.end());
            mismatches += expected != actual;
        }
        // Adversarial: for each object that crosses a 32 m line in x, a box over only its part past the line,
        // which is where a grid or tree that bounded its cells too tightly would lose it.
        auto crossings = 0;
        for (uint32_t i = 0; i < object_count && crossings < 400; ++i) {
            const auto box = object_box(i);
            const auto line = std::floor(box.hi.x / 32) * 32;
            if (line <= box.lo.x) continue;
            ++crossings;
            const auto past = box_volume({line + 0.01f, box.lo.y, box.lo.z}, box.hi);
            const auto before = box_volume(box.lo, {line - 0.01f, box.hi.y, box.hi.z});
            for (const auto& volume : {past, before}) {
                expected.clear();
                actual.clear();
                brute.query(volume, expected);
                index->query(volume, actual);
                std::sort(actual.begin(), actual.end());
                mismatches += expected != actual;
            }
        }
        if (mismatches) unexpected(std::string(index->name()) + " disagreed with brute force on " + std::to_string(mismatches) + " volumes");
    }
    std::printf("300 random 30 m volumes and 800 boxes at cell lines: every index matched brute force%s\n", failures ? " (except as reported)" : "");

    const auto& best = *std::min_element(results.begin() + 1, results.end(), [](const auto& a, const auto& b) { return a.frame_ms + a.update_ms < b.frame_ms + b.update_ms; });
    std::printf("fastest index per frame (queries and updates): %s\n", best.name.c_str());
    if (best.frame_ms + best.update_ms > 0.5 * results.front().frame_ms) unexpected("no index halves brute force's frame cost");

    // R1's models (tools/fetch_render_samples.sh) and W1's scatter (tools/fetch_world_samples.sh), where fetched.
    const auto folder = [](const char* variable, const char* fallback) { return std::filesystem::path(std::getenv(variable) ? std::getenv(variable) : fallback); };
    const auto render = folder("MAYA_RENDER_SAMPLES", MAYA_DEFAULT_SAMPLES), world = folder("MAYA_WORLD_SAMPLES", MAYA_DEFAULT_WORLD_SAMPLES);
    auto models = std::vector<std::pair<std::string, std::filesystem::path>>{};
    for (const auto* name : {"ABeautifulGame", "FlightHelmet", "DamagedHelmet"})
        if (const auto path = render / "Models" / name / "glTF" / (std::string(name) + ".gltf"); std::filesystem::exists(path)) models.emplace_back(name, path);
    for (const auto* name : {"boulder_01", "namaqualand_boulder_02", "rock_moss_set_01", "rock_moss_set_02", "tree_stump_01", "dead_tree_trunk_02"})
        if (const auto path = world / "models" / name / (std::string(name) + "_2k.gltf"); std::filesystem::exists(path)) models.emplace_back(name, path);
    if (!models.empty()) lod_report(models);
    else std::printf("\n2. LOD skipped: no samples in %s or %s\n", render.c_str(), world.c_str());
    if (failures) std::printf("\n%d unexpected results\n", failures);
    return failures ? 1 : 0;
}
