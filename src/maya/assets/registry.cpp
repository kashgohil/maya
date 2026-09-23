#include "maya/assets/registry.hpp"
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
}
AssetRegistry::AssetRegistry(std::filesystem::path root, std::unique_ptr<AssetProvider> provider)
    : m_token(detail::next_lifetime_token()), m_root(project_root(root)), m_provider(std::move(provider)) {
    if (!m_provider) throw std::invalid_argument("AssetRegistry requires a provider");
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

AssetDiagnostic AssetRegistry::register_asset(AssetRecord record) {
    if (m_loading) return {AssetError::busy,"Cannot change the asset catalog during a provider load"};
    if (!record.id.valid()) return {AssetError::invalid_id,"Asset ID must be nonzero"};
    if (record.kind != AssetKind::mesh && record.kind != AssetKind::material)
        return {AssetError::wrong_type,"Unsupported asset kind"};
    if (m_ids.contains(record.id)) return {AssetError::duplicate_id,"Duplicate asset ID " + id_text(record.id)};
    const auto full = resolve_path(record.path);
    if (!full) return {AssetError::invalid_path,"Asset path must stay inside the project: " + record.path.string()};
    const auto path_key = full->generic_string();
    if (m_paths.contains(path_key)) return {AssetError::duplicate_path,"Source already registered: " + record.path.string()};
    if (m_entries.size() >= std::numeric_limits<uint32_t>::max()) throw std::length_error("Asset slots exhausted");
    record.path = record.path.lexically_normal();
    const auto id = record.id;
    const auto slot = static_cast<uint32_t>(m_entries.size());
    m_entries.push_back(Entry{std::move(record)});
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
        else return true;
    },payload);
}

AssetRegistry::LoadOutcome AssetRegistry::load_entry(AssetId id, AssetKind kind, bool reload) {
    const auto it = m_ids.find(id);
    if (it == m_ids.end()) return {0,{AssetError::not_registered,"Asset " + id_text(id) + " is not in the project catalog"}};
    auto& entry = m_entries[it->second];
    const auto fail = [&](AssetDiagnostic diagnostic) -> LoadOutcome {
        entry.diagnostic = std::move(diagnostic);
        entry.state = usable(entry.payload) ? AssetState::ready : AssetState::failed;
        return {it->second,entry.diagnostic};
    };
    if (entry.record.kind != kind) return {it->second,{AssetError::wrong_type,"Asset type mismatch: " + entry.record.path.string()}};
    if (entry.state == AssetState::loading || m_loading)
        return {it->second,{AssetError::busy,"Nested asset loads are not supported; stage dependencies before publication"}};
    if (!reload && usable(entry.payload)) return {it->second,{}};
    if (!reload && entry.state == AssetState::failed) return {it->second,entry.diagnostic};
    if (entry.generation == std::numeric_limits<uint64_t>::max())
        return fail({AssetError::load_failed,"Asset version counter exhausted"});
    const auto path = resolve_path(entry.record.path); // recheck symlinks on every load
    if (!path) return fail({AssetError::invalid_path,"Asset path escaped the project or cannot be resolved: " + entry.record.path.string()});
    std::error_code error;
    if (!std::filesystem::is_regular_file(*path,error) || error)
        return fail({AssetError::missing_file,"Missing/unreadable asset '" + entry.record.path.string() + "'; restore the source or fix its catalog path"});

    struct LoadGuard {
        Entry& entry;
        bool& loading;
        AssetState previous;
        bool published = false;
        ~LoadGuard() { loading = false; if (!published) entry.state = previous; }
    } guard{entry,m_loading,entry.state};
    m_loading = true;
    entry.state = AssetState::loading;
    auto candidate = Payload{};
    auto diagnostic = AssetDiagnostic{};
    try {
        if (kind == AssetKind::mesh) {
            auto result = m_provider->load_mesh(*path);
            candidate = std::move(result.value); diagnostic = std::move(result.diagnostic);
        } else {
            auto result = m_provider->load_material(*path);
            candidate = std::move(result.value); diagnostic = std::move(result.diagnostic);
        }
    } catch (const std::bad_alloc&) { throw; }
    catch (const std::exception& exception) {
        diagnostic = {AssetError::load_failed,entry.record.path.string() + ": " + exception.what()};
    }
    if (!diagnostic && !usable(candidate))
        diagnostic = {AssetError::load_failed,"Provider returned no usable asset for " + entry.record.path.string()};
    if (diagnostic) {
        auto result = fail(std::move(diagnostic));
        guard.published = true;
        return result;
    }
    entry.payload = std::move(candidate);
    ++entry.generation;
    entry.diagnostic = {};
    entry.state = AssetState::ready;
    guard.published = true;
    return {it->second,{}};
}

size_t AssetRegistry::evict_unused() {
    if (m_loading) return 0;
    size_t count = 0;
    for (auto& entry : m_entries) {
        const auto unused = std::visit([](const auto& value) { return value && value.use_count() == 1; },entry.payload);
        if (!unused) continue;
        entry.payload = std::shared_ptr<const MeshAsset>{};
        entry.state = AssetState::unloaded;
        entry.diagnostic = {};
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
        if ((kind != "mesh" && kind != "material") ||
            !(input >> high >> low >> std::quoted(path)) ||
            !parse_word(high,id.high) || !parse_word(low,id.low) || !id.valid())
            return {{},{AssetError::invalid_data,"Invalid catalog entry " + std::to_string(records.size()+1)}};
        records.push_back({id,kind == "mesh" ? AssetKind::mesh : AssetKind::material,path});
    }
    if (input.bad() || !input.eof()) return {{},{AssetError::invalid_data,"I/O failure reading catalog"}};
    return {std::move(records),{}};
}
void write_asset_catalog(std::ostream& output, const std::vector<AssetRecord>& records) {
    // Use a private stream so formatting flags on the caller's stream are untouched.
    auto text = std::ostringstream{};
    text << "maya-assets 1\n";
    for (const auto& record : records)
        text << (record.kind == AssetKind::mesh ? "mesh" : "material") << ' ' << std::hex
             << record.id.high << ' ' << record.id.low << ' ' << std::quoted(record.path.generic_string()) << '\n';
    output << text.str();
    if (!output) throw std::runtime_error("Cannot write asset catalog");
}
} // namespace maya
