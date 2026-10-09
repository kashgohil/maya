#include "maya/assets/registry.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <cctype>
#include <fstream>
#include <charconv>
#include <iomanip>
#include <sstream>

namespace maya {
namespace {
std::filesystem::path project_root(const std::filesystem::path& path) {
    std::error_code error;
    const auto root = std::filesystem::canonical(path,error);
    if (error || !std::filesystem::is_directory(root,error) || error)
        throw std::invalid_argument("Asset project root must be an existing directory: " + path.string());
    return root;
}
std::string id_text(AssetId id) {
    auto text = std::ostringstream{};
    text << std::hex << id.high << ':' << id.low;
    return text.str();
}
ResidencyCategory category_of(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::mesh: return ResidencyCategory::meshes;
    case AssetKind::texture: return ResidencyCategory::textures;
    case AssetKind::environment: return ResidencyCategory::environments;
    case AssetKind::skin:
    case AssetKind::animation: return ResidencyCategory::animation;
    case AssetKind::material:
    case AssetKind::script: break;
    }
    return ResidencyCategory::other;
}
/// A version's bytes: the device's from its descriptors, the CPU's from what it keeps.
ResidentBytes bytes_of(const AssetValue& payload) noexcept {
    return std::visit([](const auto& value) {
        auto bytes = ResidentBytes{};
        if (!value) return bytes;
        using T = typename std::decay_t<decltype(value)>::element_type;
        if constexpr (std::same_as<T, const MeshAsset>) {
            bytes.gpu = value->mesh().gpu_bytes();
            bytes.cpu = value->geometry().positions.size() * sizeof(math::Vec3) + value->geometry().indices.size() * sizeof(uint32_t);
        } else if constexpr (std::same_as<T, const TextureAsset> || std::same_as<T, const EnvironmentAsset>) {
            bytes.gpu = value->gpu_bytes();
        } else if constexpr (std::same_as<T, const SkinAsset>) {
            bytes.cpu = value->inverse_bind.size() * sizeof(math::Mat4);
            for (const auto& joint : value->joints) bytes.cpu += sizeof(joint) + joint.capacity();
        } else if constexpr (std::same_as<T, const AnimationAsset>) {
            for (const auto& channel : value->channels)
                bytes.cpu += sizeof(channel) + channel.target.capacity() + (channel.times.size() + channel.values.size()) * sizeof(float);
        } else if constexpr (std::same_as<T, const ScriptAsset>) {
            bytes.cpu = value->source.size();
        } else {
            bytes.cpu = sizeof(T);
        }
        return bytes;
    }, payload);
}
bool leased(const AssetValue& payload) noexcept {
    return std::visit([](const auto& value) { return value && value.use_count() > 1; }, payload); // the cache's own is one
}
std::string mib_text(size_t bytes) {
    auto text = std::ostringstream{};
    text << std::fixed << std::setprecision(1) << double(bytes) / double(1 << 20) << " MiB";
    return text.str();
}
}
AssetRegistry::AssetRegistry(std::filesystem::path root, std::unique_ptr<AssetProvider> provider)
    : m_token(detail::next_lifetime_token()), m_root(project_root(root)), m_provider(std::move(provider)) {
    if (!m_provider) throw std::invalid_argument("AssetRegistry requires a provider");
}
AssetRegistry::~AssetRegistry() {
    // Jobs first: cancelled, and waited for, before the provider and the entries they were preparing go.
    m_jobs.cancel();
    m_jobs.wait();
}

std::optional<std::filesystem::path> AssetRegistry::resolve_path(const std::filesystem::path& path) const {
    if (path.empty() || path.is_absolute() || path.has_root_name()) return std::nullopt;
    const auto normalized = path.lexically_normal();
    for (const auto& part : normalized) if (part == "..") return std::nullopt;
    std::error_code error;
    const auto full = std::filesystem::weakly_canonical(m_root/normalized,error);
    if (error) return std::nullopt;
    const auto relative = full.lexically_relative(m_root);
    if (relative.empty() || relative == ".") return std::nullopt;
    for (const auto& part : relative) if (part == "..") return std::nullopt;
    return full;
}

const char* asset_kind_name(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::mesh: return "mesh";
    case AssetKind::material: return "material";
    case AssetKind::texture: return "texture";
    case AssetKind::environment: return "environment";
    case AssetKind::skin: return "skin";
    case AssetKind::animation: return "animation";
    case AssetKind::script: break;
    }
    return "script";
}

AssetLoadResult<TextureAsset> AssetProvider::load_texture(const std::filesystem::path& path) {
    return {nullptr, {AssetError::load_failed, "This asset provider does not load textures: " + path.string()}};
}

AssetLoadResult<SkinAsset> AssetProvider::load_imported_skin(const std::filesystem::path& source, std::string_view part) {
    return {nullptr, {AssetError::load_failed, "This asset provider does not load skins: " + source.string() + "#" + std::string(part)}};
}
AssetLoadResult<AnimationAsset> AssetProvider::load_imported_animation(const std::filesystem::path& source, std::string_view part) {
    return {nullptr, {AssetError::load_failed, "This asset provider does not load animations: " + source.string() + "#" + std::string(part)}};
}
AssetLoadResult<SkinAsset> AssetProvider::load_skin(const std::filesystem::path& path) {
    return {nullptr, {AssetError::load_failed, "This asset provider does not load skins: " + path.string()}};
}
AssetLoadResult<AnimationAsset> AssetProvider::load_animation(const std::filesystem::path& path) {
    return {nullptr, {AssetError::load_failed, "This asset provider does not load animations: " + path.string()}};
}

AssetLoadResult<EnvironmentAsset> AssetProvider::load_environment(const std::filesystem::path& path) {
    return {nullptr, {AssetError::load_failed, "This asset provider does not load environments: " + path.string()}};
}

AssetLoadResult<MeshAsset> AssetProvider::load_imported_mesh(const std::filesystem::path& source, std::string_view part) {
    return {nullptr, {AssetError::load_failed, "This asset provider does not load imported meshes: " + source.string() + "#" + std::string(part)}};
}

AssetLoadResult<TextureAsset> AssetProvider::load_imported_texture(const std::filesystem::path& source, std::string_view part) {
    return {nullptr, {AssetError::load_failed, "This asset provider does not load imported textures: " + source.string() + "#" + std::string(part)}};
}

std::pair<AssetValue, AssetDiagnostic> AssetProvider::load_now(const AssetLoadRequest& request) {
    const auto& path = request.path;
    const auto& part = request.part;
    const auto take = [](auto result) -> std::pair<AssetValue, AssetDiagnostic> { return {std::move(result.value), std::move(result.diagnostic)}; };
    switch (request.kind) {
    case AssetKind::mesh: return take(part.empty() ? load_mesh(path) : load_imported_mesh(path, part));
    case AssetKind::texture: return take(part.empty() ? load_texture(path) : load_imported_texture(path, part));
    case AssetKind::skin: return take(part.empty() ? load_skin(path) : load_imported_skin(path, part));
    case AssetKind::animation: return take(part.empty() ? load_animation(path) : load_imported_animation(path, part));
    case AssetKind::material: return take(load_material(path));
    case AssetKind::environment: return take(load_environment(path));
    case AssetKind::script: return take(load_script(path));
    }
    return {std::shared_ptr<const MeshAsset>{}, {AssetError::wrong_type, "Unsupported asset kind"}};
}
PreparedAsset AssetProvider::prepare(const AssetLoadRequest& request) {
    return {{}, 0, [this, request] { return load_now(request); }};
}

AssetSourcePath split_asset_path(const std::filesystem::path& path) {
    const auto text = path.generic_string();
    const auto mark = text.rfind('#');
    if (mark == std::string::npos) return {path, {}};
    auto file = text.substr(0, mark);
    auto extension = std::filesystem::path(file).extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (extension != ".gltf" && extension != ".glb") return {path, {}};
    return {std::filesystem::path(file), text.substr(mark + 1)};
}

AssetLoadResult<ScriptAsset> AssetProvider::load_script(const std::filesystem::path& path) {
    auto file = std::ifstream(path, std::ios::binary);
    if (!file) return {nullptr, {AssetError::missing_file, "Cannot read script " + path.string()}};
    auto script = std::make_shared<ScriptAsset>();
    script->source.assign(std::istreambuf_iterator<char>(file), {});
    if (file.bad()) return {nullptr, {AssetError::load_failed, "Cannot read script " + path.string()}};
    return {std::move(script), {}};
}

AssetDiagnostic AssetRegistry::register_asset(AssetRecord record) {
    if (m_loading) return {AssetError::busy,"Cannot change the asset catalog during a provider load"};
    if (!record.id.valid()) return {AssetError::invalid_id,"Asset ID must be nonzero"};
    if (record.kind != AssetKind::mesh && record.kind != AssetKind::material && record.kind != AssetKind::script &&
        record.kind != AssetKind::texture && record.kind != AssetKind::environment && record.kind != AssetKind::skin &&
        record.kind != AssetKind::animation)
        return {AssetError::wrong_type,"Unsupported asset kind"};
    if (m_ids.contains(record.id)) return {AssetError::duplicate_id,"Duplicate asset ID " + id_text(record.id)};
    const auto source = split_asset_path(record.path);
    if (!source.part.empty() && record.kind != AssetKind::mesh && record.kind != AssetKind::texture && record.kind != AssetKind::skin &&
        record.kind != AssetKind::animation)
        return {AssetError::invalid_path,"Only meshes, textures, skins, and animations can be parts of an imported file: " + record.path.string()};
    const auto full = resolve_path(source.file);
    if (!full) return {AssetError::invalid_path,"Asset path must stay inside the project: " + record.path.string()};
    const auto path_key = full->generic_string() + (source.part.empty() ? "" : "#" + source.part);
    if (m_paths.contains(path_key)) return {AssetError::duplicate_path,"Source already registered: " + record.path.string()};
    if (m_entries.size() >= std::numeric_limits<uint32_t>::max()) throw std::length_error("Asset slots exhausted");
    record.path = record.path.lexically_normal();
    const auto id = record.id;
    const auto slot = static_cast<uint32_t>(m_entries.size());
    auto added = Entry{};
    added.category = category_of(record.kind);
    added.record = std::move(record);
    m_entries.push_back(std::move(added));
    try {
        m_ids.emplace(id,slot);
        try { m_paths.emplace(path_key,id); }
        catch (...) { m_ids.erase(id); throw; }
    } catch (...) { m_entries.pop_back(); throw; }
    return {};
}

std::vector<AssetRecord> AssetRegistry::records() const {
    auto records = std::vector<AssetRecord>{};
    records.reserve(m_entries.size());
    for (const auto& entry : m_entries) records.push_back(entry.record);
    return records;
}
AssetResidency AssetRegistry::residency() const noexcept {
    auto result = AssetResidency{};
    result.entries = m_entries.size();
    for (const auto& entry : m_entries) {
        if (entry.state == AssetState::unloaded) ++result.unloaded;
        else if (entry.state == AssetState::loading) ++result.loading;
        else if (entry.state == AssetState::ready) ++result.ready;
        else if (entry.state == AssetState::failed) ++result.failed;
        if (leased(entry.payload)) result.leased_bytes[size_t(entry.category)] += entry.bytes.total();
        std::visit([&](const auto& value) {
            if (!value) return;
            if (value.use_count() > 1) ++result.leased; // the cache's own reference is one
            if constexpr (std::same_as<typename std::decay_t<decltype(value)>::element_type, const MeshAsset>) {
                ++result.meshes;
                result.mesh_gpu_bytes += value->mesh().gpu_bytes();
                const auto& geometry = value->geometry();
                result.mesh_cpu_bytes += geometry.positions.size() * sizeof(math::Vec3) + geometry.indices.size() * sizeof(uint32_t);
            } else if constexpr (std::same_as<typename std::decay_t<decltype(value)>::element_type, const MaterialAsset>) {
                ++result.materials;
            } else if constexpr (std::same_as<typename std::decay_t<decltype(value)>::element_type, const TextureAsset>) {
                ++result.textures;
                result.texture_gpu_bytes += value->gpu_bytes();
            } else if constexpr (std::same_as<typename std::decay_t<decltype(value)>::element_type, const EnvironmentAsset>) {
                ++result.environments;
                result.environment_gpu_bytes += value->gpu_bytes();
            } else if constexpr (std::same_as<typename std::decay_t<decltype(value)>::element_type, const SkinAsset>) {
                ++result.skins;
            } else if constexpr (std::same_as<typename std::decay_t<decltype(value)>::element_type, const AnimationAsset>) {
                ++result.animations;
            } else {
                ++result.scripts;
            }
        }, entry.payload);
    }
    result.bytes = m_resident;
    result.budgets = m_budgets;
    return result;
}

std::vector<ResidentAsset> AssetRegistry::largest(size_t count) const {
    auto result = std::vector<ResidentAsset>{};
    for (const auto& entry : m_entries)
        if (usable(entry.payload))
            result.push_back({entry.record.path.generic_string(), entry.category, entry.bytes, leased(entry.payload)});
    const auto keep = std::min(count, result.size());
    std::ranges::partial_sort(result, result.begin() + ptrdiff_t(keep), std::ranges::greater{}, [](const ResidentAsset& asset) { return asset.bytes.total(); });
    result.resize(keep);
    return result;
}

void AssetRegistry::touch(uint32_t slot) const {
    const auto& entry = m_entries[slot];
    auto& order = m_lru[size_t(entry.category)];
    order.erase({entry.queued, slot});
    entry.last_use = entry.last_wanted = entry.queued = ++m_clock;
    order.insert({entry.queued, slot});
}

void AssetRegistry::requeue(uint32_t slot) const {
    const auto& entry = m_entries[slot];
    auto& order = m_lru[size_t(entry.category)];
    order.erase({entry.queued, slot});
    entry.queued = ++m_clock;
    order.insert({entry.queued, slot});
}

void AssetRegistry::set_payload(uint32_t slot, Payload payload) {
    auto& entry = m_entries[slot];
    auto& total = m_resident[size_t(entry.category)];
    auto& order = m_lru[size_t(entry.category)];
    total.cpu -= entry.bytes.cpu;
    total.gpu -= entry.bytes.gpu;
    order.erase({entry.queued, slot});
    entry.payload = std::move(payload);
    entry.bytes = bytes_of(entry.payload);
    total.cpu += entry.bytes.cpu;
    total.gpu += entry.bytes.gpu;
    if (std::visit([](const auto& value) { return static_cast<bool>(value); }, entry.payload)) {
        entry.last_use = entry.queued = ++m_clock; // a new version counts as used now
        order.insert({entry.queued, slot});
    }
}

void AssetRegistry::release(uint32_t slot) {
    auto& entry = m_entries[slot];
    set_payload(slot, std::shared_ptr<const MeshAsset>{});
    entry.state = AssetState::unloaded;
    entry.diagnostic = {};
    ++m_release.released;
}

void AssetRegistry::maintain(uint64_t used_since) {
    if (m_loading) return;
    const auto start = std::chrono::steady_clock::now();
    constexpr size_t examine = 64; // versions looked at an update, over every category: never a sweep of the catalog
    auto examined = size_t{0};
    for (size_t c = 0; c < residency_category_count; ++c) {
        const auto category = ResidencyCategory(c);
        const auto budget = m_budgets.bytes[c];
        auto over = budgeted(category) && budget > 0 && m_resident[c].total() > budget;
        if (!over) {
            m_release.over_budget[c] = false;
            m_passed[c] = 0;
            continue;
        }
        auto& order = m_lru[c];
        auto held = std::vector<uint32_t>{}; // in use: moved to the back, so the next look starts past them
        for (auto it = order.begin(); it != order.end() && over && examined < examine; ++examined) {
            const auto slot = (it++)->second;
            const auto& entry = m_entries[slot];
            if (entry.pending) continue; // a reload in flight replaces it soon
            if (leased(entry.payload) || entry.last_use > used_since) { // held, or drawn by the last frame
                held.push_back(slot);
                continue;
            }
            release(slot);
            ++m_release.released_for_budget;
            m_passed[c] = 0;
            over = m_resident[c].total() > budget;
        }
        for (const auto slot : held) requeue(slot);
        // Passed over in use, twice a whole queue's worth since anything was released (two looks at each, so
        // not merely what a load just brought in), and still over: it is all in use.
        m_passed[c] += held.size();
        const auto exhausted = over && m_passed[c] >= 2 * order.size();
        if (exhausted && !m_release.over_budget[c]) {
            // Say so once, naming the largest holders.
            auto users = std::vector<std::pair<size_t, uint32_t>>{};
            for (const auto& [queued, slot] : order) users.emplace_back(m_entries[slot].bytes.total(), slot);
            std::ranges::sort(users, std::ranges::greater{});
            auto text = std::string("Resident ") + residency_category_name(category) + " are " + mib_text(m_resident[c].total()) + ", over their " +
                        mib_text(budget) + " budget, and all of it is in use. Largest: ";
            for (size_t i = 0; i < std::min<size_t>(3, users.size()); ++i)
                text += (i ? ", " : "") + m_entries[users[i].second].record.path.generic_string() + " (" + mib_text(users[i].first) + ")";
            m_warnings.push_back(std::move(text));
        }
        if (exhausted) m_release.over_budget[c] = true;
        else if (!over) m_release.over_budget[c] = false;
    }
    m_release.last_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_release.longest_ms = std::max(m_release.longest_ms, m_release.last_ms);
}

std::optional<AssetInfo> AssetRegistry::info(AssetId id) const {
    const auto it = m_ids.find(id);
    if (it == m_ids.end()) return std::nullopt;
    const auto& entry = m_entries[it->second];
    return AssetInfo{entry.record,entry.state,entry.generation,entry.diagnostic};
}
bool AssetRegistry::usable(const Payload& payload) noexcept {
    return std::visit([](const auto& value) {
        if (!value) return false;
        if constexpr (std::same_as<typename std::decay_t<decltype(value)>::element_type,const MeshAsset>)
            return value->mesh().valid();
        else if constexpr (std::same_as<typename std::decay_t<decltype(value)>::element_type,const TextureAsset>)
            return value->valid();
        else if constexpr (std::same_as<typename std::decay_t<decltype(value)>::element_type,const EnvironmentAsset>)
            return value->valid();
        else return true;
    },payload);
}

namespace detail {
struct AssetRequestState {
    AssetId id;
    AssetState state = AssetState::loading; // this asset's own load: loading, ready, or failed
    AssetDiagnostic diagnostic;
    bool cancelled = false;
    std::vector<AssetRequest> children; // a material's textures
};
} // namespace detail

AssetId AssetRequest::id() const noexcept { return m_state ? m_state->id : AssetId{}; }
AssetState AssetRequest::state() const noexcept {
    if (!m_state || m_state->cancelled || m_state->state == AssetState::failed) return AssetState::failed;
    auto loading = m_state->state == AssetState::loading;
    for (const auto& child : m_state->children) {
        const auto state = child.state();
        if (state == AssetState::failed) return AssetState::failed;
        loading = loading || state == AssetState::loading;
    }
    return loading ? AssetState::loading : AssetState::ready;
}
AssetDiagnostic AssetRequest::diagnostic() const {
    if (!m_state) return {AssetError::not_registered, "No asset was requested"};
    if (m_state->cancelled) return {AssetError::cancelled, "The request was cancelled"};
    if (m_state->diagnostic) return m_state->diagnostic;
    for (const auto& child : m_state->children)
        if (auto diagnostic = child.diagnostic()) return diagnostic;
    return {};
}
void AssetRequest::cancel() const noexcept {
    if (!m_state) return;
    m_state->cancelled = true;
    for (const auto& child : m_state->children) child.cancel();
}

std::pair<uint32_t, AssetDiagnostic> AssetRegistry::find(AssetId id, AssetKind kind) const {
    const auto it = m_ids.find(id);
    if (it == m_ids.end()) return {0, {AssetError::not_registered, "Asset " + id_text(id) + " is not in the project catalog"}};
    const auto& entry = m_entries[it->second];
    if (entry.record.kind != kind) return {it->second, {AssetError::wrong_type, "Asset type mismatch: " + entry.record.path.string()}};
    if (m_loading) return {it->second, {AssetError::busy, "Nested asset loads are not supported; stage dependencies before publication"}};
    return {it->second, {}};
}

namespace {
bool wanted(const std::vector<std::weak_ptr<detail::AssetRequestState>>& requests) {
    return std::ranges::any_of(requests, [](const auto& weak) {
        const auto request = weak.lock();
        return request && !request->cancelled;
    });
}
} // namespace

AssetRequest AssetRegistry::request_entry(AssetId id, AssetKind kind, JobTier tier, bool reload) {
    auto request = std::make_shared<detail::AssetRequestState>();
    request->id = id;
    const auto [slot, problem] = find(id, kind);
    if (problem) {
        request->state = AssetState::failed;
        request->diagnostic = problem;
        return AssetRequest(request);
    }
    auto& entry = m_entries[slot];
    entry.last_wanted = ++m_clock;
    if (!reload && !entry.pending) {
        if (usable(entry.payload)) {
            request->state = AssetState::ready;
            attach(entry, request, tier);
            return AssetRequest(request);
        }
        if (entry.state == AssetState::failed) {
            request->state = AssetState::failed;
            request->diagnostic = entry.diagnostic;
            return AssetRequest(request);
        }
    }
    if (auto diagnostic = start_load(slot, reload, false, tier)) {
        request->state = AssetState::failed;
        request->diagnostic = std::move(diagnostic);
        return AssetRequest(request);
    }
    m_entries[slot].pending->requests.push_back(request);
    return AssetRequest(request);
}

// A ready material's request includes its textures' requests: the tree loads, fails, and is cancelled as one.
void AssetRegistry::attach(Entry& entry, const std::shared_ptr<detail::AssetRequestState>& request, JobTier tier) {
    if (entry.record.kind != AssetKind::material || !usable(entry.payload)) return;
    const auto& material = *std::get<std::shared_ptr<const MaterialAsset>>(entry.payload);
    for (const auto texture : {material.base_color_texture, material.metallic_roughness_texture, material.normal_texture,
                               material.occlusion_texture, material.emissive_texture})
        if (texture.valid()) request->children.push_back(request_entry(texture.id, AssetKind::texture, tier, false));
}

AssetDiagnostic AssetRegistry::start_held(uint32_t slot, JobTier tier) {
    auto& entry = m_entries[slot];
    if (entry.pending) {
        if (!entry.pending->held) ++m_stats.merged; // joined once; later frames find it held
        entry.pending->held = true;
        return {};
    }
    if (entry.state == AssetState::failed) return entry.diagnostic;
    return start_load(slot, false, true, tier);
}

AssetDiagnostic AssetRegistry::start_load(uint32_t slot, bool reload, bool held, JobTier tier) {
    auto& entry = m_entries[slot];
    auto requests = std::vector<std::weak_ptr<detail::AssetRequestState>>{};
    auto previous = entry.state;
    if (entry.pending) {
        if (!reload || entry.pending->reload) {
            entry.pending->held = entry.pending->held || held;
            ++m_stats.merged;
            return {};
        }
        // A reload supersedes a plain load in flight, whose source may have changed since it began; the
        // old load's completion will be discarded, and its requests follow the new one.
        entry.pending->job.cancel();
        requests = std::move(entry.pending->requests);
        held = held || entry.pending->held;
        previous = entry.pending->previous;
        entry.pending.reset();
        std::erase(m_in_flight, slot);
    }
    const auto fail = [&](AssetDiagnostic diagnostic) {
        entry.diagnostic = diagnostic;
        entry.state = usable(entry.payload) ? AssetState::ready : AssetState::failed;
        for (const auto& weak : requests)
            if (const auto request = weak.lock()) {
                request->state = AssetState::failed;
                request->diagnostic = diagnostic;
            }
        return diagnostic;
    };
    if (entry.generation == std::numeric_limits<uint64_t>::max()) return fail({AssetError::load_failed, "Asset version counter exhausted"});
    const auto source = split_asset_path(entry.record.path);
    const auto path = resolve_path(source.file); // recheck symlinks on every load
    if (!path) return fail({AssetError::invalid_path, "Asset path escaped the project or cannot be resolved: " + entry.record.path.string()});
    std::error_code error;
    if (!std::filesystem::is_regular_file(*path, error) || error)
        return fail({AssetError::missing_file, "Missing/unreadable asset '" + entry.record.path.string() + "'; restore the source or fix its catalog path"});

    auto load = PendingLoad{};
    load.generation = ++m_next_generation;
    load.reload = reload;
    load.held = held;
    load.tier = tier;
    load.previous = previous;
    load.start = std::chrono::steady_clock::now();
    load.requests = std::move(requests);
    const auto generation = load.generation;
    const auto label = entry.record.path.string();
    load.job = m_jobs.submit(tier, [this, provider = m_provider.get(), sink = m_completions.sink(), request = AssetLoadRequest{entry.record.kind, *path, source.part, tier, {}},
                                    slot, generation, label](JobContext& context) {
        if (context.cancelled()) return;
        auto step = request;
        step.cancelled = [&context] { return context.cancelled(); };
        auto prepared = std::make_shared<PreparedAsset>();
        try {
            *prepared = provider->prepare(step);
        } catch (const std::exception& exception) {
            *prepared = {{AssetError::load_failed, label + ": " + exception.what()}, 0, {}};
        } catch (...) {
            *prepared = {{AssetError::load_failed, label + ": an exception that is not a std::exception"}, 0, {}};
        }
        if (context.cancelled()) return;
        // Applied on the owner thread by update or a wait, if this load is still the entry's.
        sink.post(slot, generation, [this, slot, prepared] {
            auto& entry = m_entries[slot];
            entry.pending->prepared = prepared;
            m_prepared.push_back(slot);
        });
    });
    if (!usable(entry.payload)) entry.state = AssetState::loading;
    entry.pending = std::move(load);
    m_in_flight.push_back(slot);
    ++m_stats.started;
    return {};
}

void AssetRegistry::apply_completions() {
    m_completions.drain(std::chrono::hours(1), [this](uint64_t slot, uint64_t generation) {
        const auto current = slot < m_entries.size() && m_entries[slot].pending && m_entries[slot].pending->generation == generation &&
                             !m_entries[slot].pending->prepared;
        if (!current) ++m_stats.discarded;
        return current;
    });
}

void AssetRegistry::cancel_load(uint32_t slot) {
    auto& entry = m_entries[slot];
    entry.pending->job.cancel();
    for (const auto& weak : entry.pending->requests)
        if (const auto request = weak.lock()) {
            request->state = AssetState::failed;
            request->diagnostic = {AssetError::cancelled, "The load of " + entry.record.path.string() + " was cancelled"};
        }
    entry.state = usable(entry.payload) ? AssetState::ready : entry.pending->previous == AssetState::failed ? AssetState::failed : AssetState::unloaded;
    entry.pending.reset();
    std::erase(m_in_flight, slot);
    ++m_stats.cancelled;
}

void AssetRegistry::finalize(uint32_t slot) {
    auto& entry = m_entries[slot];
    auto load = std::move(*entry.pending);
    entry.pending.reset();
    std::erase(m_in_flight, slot);
    const auto& prepared = *load.prepared;
    auto value = Payload{};
    auto diagnostic = prepared.diagnostic;
    if (!diagnostic && !prepared.finalize) diagnostic = {AssetError::load_failed, "The provider prepared nothing to finalize for " + entry.record.path.string()};
    if (!diagnostic) {
        m_loading = true;
        try {
            auto [finalized, problem] = prepared.finalize();
            value = std::move(finalized);
            diagnostic = std::move(problem);
        } catch (const std::bad_alloc&) {
            m_loading = false;
            entry.state = usable(entry.payload) ? AssetState::ready : load.previous; // as if the load never began
            throw;
        } catch (const std::exception& exception) {
            diagnostic = {AssetError::load_failed, entry.record.path.string() + ": " + exception.what()};
        }
        m_loading = false;
    }
    if (!diagnostic && !usable(value)) diagnostic = {AssetError::load_failed, "Provider returned no usable asset for " + entry.record.path.string()};
    if (diagnostic) {
        // A failed reload keeps the version in use.
        entry.diagnostic = diagnostic;
        entry.state = usable(entry.payload) ? AssetState::ready : AssetState::failed;
        ++m_stats.failed;
    } else {
        set_payload(slot, std::move(value));
        ++entry.generation;
        entry.diagnostic = {};
        entry.state = AssetState::ready;
        ++m_stats.finalized;
        m_stats.bytes_finalized += prepared.upload_bytes;
        const auto latency = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load.start).count();
        if (m_latencies.size() < 256) m_latencies.push_back(latency);
        else m_latencies[m_latency_next++ % 256] = latency;
    }
    for (const auto& weak : load.requests)
        if (const auto request = weak.lock()) {
            request->state = diagnostic ? AssetState::failed : AssetState::ready;
            request->diagnostic = diagnostic;
            if (!diagnostic && !request->cancelled) attach(entry, request, load.tier);
        }
    // A material kept by the registry starts its maps' loads now, not when it is next drawn, on its own
    // tier: an explicit wait's maps are about to be waited for too.
    if (!diagnostic && load.held && entry.record.kind == AssetKind::material) {
        const auto material = std::get<std::shared_ptr<const MaterialAsset>>(entry.payload);
        for (const auto texture : {material->base_color_texture, material->metallic_roughness_texture, material->normal_texture,
                                   material->occlusion_texture, material->emissive_texture})
            if (const auto [child, problem] = find(texture.id, AssetKind::texture); !problem)
                if (!m_entries[child].pending && !usable(m_entries[child].payload) && m_entries[child].state != AssetState::failed)
                    (void)start_held(child, load.tier);
    }
}

AssetLoadStats AssetRegistry::update(AssetLoadBudget budget) {
    const auto start = std::chrono::steady_clock::now();
    apply_completions();
    // Loads no one wants any more: not kept by the registry, and every request gone or cancelled.
    for (auto i = m_in_flight.size(); i-- > 0;) {
        const auto slot = m_in_flight[i];
        const auto& load = *m_entries[slot].pending;
        if (!load.held && !wanted(load.requests)) cancel_load(slot);
    }
    size_t count = 0, bytes = 0;
    while (!m_prepared.empty()) {
        const auto slot = m_prepared.front();
        auto& entry = m_entries[slot];
        if (!entry.pending || !entry.pending->prepared) { // finalized by a wait, cancelled, or superseded
            m_prepared.pop_front();
            continue;
        }
        const auto upload = entry.pending->prepared->upload_bytes;
        if (count > 0 && (std::chrono::steady_clock::now() - start >= budget.time || bytes + upload > budget.bytes)) break;
        m_prepared.pop_front();
        finalize(slot);
        ++count;
        bytes += upload;
    }
    m_stats.last_finalized = count;
    m_stats.last_bytes = bytes;
    m_stats.last_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_stats.longest_ms = std::max(m_stats.longest_ms, m_stats.last_ms);
    maintain(std::exchange(m_update_clock, m_clock));
    return load_stats();
}

AssetLoadStats AssetRegistry::load_stats() const {
    auto stats = m_stats;
    stats.in_flight = m_in_flight.size();
    stats.prepared = size_t(std::ranges::count_if(m_in_flight, [&](uint32_t slot) { return m_entries[slot].pending->prepared != nullptr; }));
    if (!m_latencies.empty()) {
        auto sorted = m_latencies;
        std::ranges::sort(sorted);
        const auto at = [&](double p) { return sorted[std::min(sorted.size() - 1, size_t(std::ceil(p * double(sorted.size()))) - 1)]; };
        stats.latency_p50_ms = at(0.5);
        stats.latency_p95_ms = at(0.95);
        stats.latency_max_ms = sorted.back();
    }
    return stats;
}

AssetDiagnostic AssetRegistry::load_and_wait(uint32_t slot, bool reload) {
    auto& entry = m_entries[slot];
    if (!reload && usable(entry.payload)) return {};
    if (!reload && !entry.pending && entry.state == AssetState::failed) return entry.diagnostic;
    if (auto diagnostic = start_load(slot, reload, true, JobTier::frame)) return diagnostic;
    return wait_slot(slot);
}

AssetDiagnostic AssetRegistry::wait_slot(uint32_t slot) {
    auto& entry = m_entries[slot];
    ++m_stats.waited;
    if (m_in_frame && m_explicit == 0) ++m_stats.waited_in_frames;
    while (entry.pending) {
        if (entry.pending->prepared) {
            finalize(slot);
            break;
        }
        const auto job = entry.pending->job;
        job.wait(); // the owner thread blocks here: an explicit wait
        apply_completions();
        if (entry.pending && !entry.pending->prepared && job.done()) {
            // The job ended without a result (it was cancelled): the load cannot finish.
            cancel_load(slot);
            return {AssetError::cancelled, "The load of " + entry.record.path.string() + " was cancelled"};
        }
    }
    return entry.diagnostic;
}

AssetState AssetRegistry::wait(const AssetRequest& request) {
    if (!request) return AssetState::failed;
    for (;;) {
        if (request.done()) return request.state();
        // Every load still in flight in the tree; children appear when a material is finalized.
        auto slots = std::vector<uint32_t>{};
        const std::function<void(const detail::AssetRequestState&)> collect = [&](const detail::AssetRequestState& node) {
            if (node.cancelled) return;
            if (node.state == AssetState::loading)
                if (const auto it = m_ids.find(node.id); it != m_ids.end() && m_entries[it->second].pending) slots.push_back(it->second);
            for (const auto& child : node.children) collect(*child.m_state);
        };
        collect(*request.m_state);
        if (slots.empty()) return request.state();
        for (const auto slot : slots)
            if (m_entries[slot].pending) wait_slot(slot);
    }
}

void AssetRegistry::wait_idle() {
    while (!m_in_flight.empty()) wait_slot(m_in_flight.front());
}

AssetDiagnostic AssetRegistry::publish(AssetRef<MaterialAsset> ref, MaterialAsset value) {
    const auto it = m_ids.find(ref.id);
    if (it == m_ids.end()) return {AssetError::not_registered,"Asset " + id_text(ref.id) + " is not in the project catalog"};
    auto& entry = m_entries[it->second];
    if (entry.record.kind != AssetKind::material) return {AssetError::wrong_type,"Asset type mismatch: " + entry.record.path.string()};
    if (m_loading) return {AssetError::busy,"Nested asset loads are not supported; stage dependencies before publication"};
    if (entry.generation == std::numeric_limits<uint64_t>::max()) return {AssetError::load_failed,"Asset version counter exhausted"};
    if (entry.pending) cancel_load(it->second); // the edit wins over a load of the file in flight
    set_payload(it->second, std::make_shared<const MaterialAsset>(std::move(value)));
    ++entry.generation;
    entry.diagnostic = {};
    entry.state = AssetState::ready;
    return {};
}

size_t AssetRegistry::evict_unused(uint64_t clock) {
    if (m_loading) return 0;
    size_t count = 0;
    const auto boundary = clock != std::numeric_limits<uint64_t>::max();
    for (uint32_t slot = 0; slot < m_entries.size(); ++slot) {
        const auto& entry = m_entries[slot];
        if (entry.last_wanted > clock) continue;
        // A load the registry keeps for no one since the boundary (what a closed scene was drawing) would
        // arrive unused: it is cancelled. One someone requests goes on.
        if (entry.pending && boundary && entry.pending->held && !wanted(entry.pending->requests)) cancel_load(slot);
        if (entry.pending) continue;
        const auto unused = std::visit([](const auto& value) { return value && value.use_count() == 1; },entry.payload);
        if (!unused) continue;
        release(slot);
        ++count;
    }
    return count;
}

AssetCatalogResult read_asset_catalog(std::istream& input) {
    std::string magic;
    unsigned version = 0;
    if (!(input >> magic >> version) || magic != "maya-assets" || version != 1)
        return {{},{AssetError::invalid_data,"Expected maya-assets 1 catalog header"}};
    std::vector<AssetRecord> records;
    std::string kind,path,high,low;
    const auto parse_word = [](const std::string& word, uint64_t& value) {
        const auto parsed = std::from_chars(word.data(),word.data()+word.size(),value,16);
        return parsed.ec == std::errc{} && parsed.ptr == word.data()+word.size();
    };
    while (input >> kind) {
        AssetId id;
        if ((kind != "mesh" && kind != "material" && kind != "script" && kind != "texture" && kind != "environment" && kind != "skin" &&
             kind != "animation") ||
            !(input >> high >> low >> std::quoted(path)) ||
            !parse_word(high,id.high) || !parse_word(low,id.low) || !id.valid())
            return {{},{AssetError::invalid_data,"Invalid catalog entry " + std::to_string(records.size()+1)}};
        records.push_back({id,kind == "mesh" ? AssetKind::mesh : kind == "material" ? AssetKind::material :
                              kind == "script" ? AssetKind::script : kind == "texture" ? AssetKind::texture :
                              kind == "skin" ? AssetKind::skin : kind == "animation" ? AssetKind::animation :
                              AssetKind::environment,path});
    }
    if (input.bad() || !input.eof()) return {{},{AssetError::invalid_data,"I/O failure reading catalog"}};
    return {std::move(records),{}};
}
void write_asset_catalog(std::ostream& output, const std::vector<AssetRecord>& records) {
    // Use a private stream so formatting flags on the caller's stream are untouched.
    auto text = std::ostringstream{};
    text << "maya-assets 1\n";
    for (const auto& record : records)
        text << asset_kind_name(record.kind) << ' ' << std::hex
             << record.id.high << ' ' << record.id.low << ' ' << std::quoted(record.path.generic_string()) << '\n';
    output << text.str();
    if (!output) throw std::runtime_error("Cannot write asset catalog");
}
} // namespace maya
