#include "maya/assets/cook_cache.hpp"
#include "maya/assets/project.hpp"
#include "maya/core/file_replace.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

namespace maya {
namespace {
// An entry: "MAYACOOK", the format version (uint32), the payload's size (uint64), the payload's digest,
// then the payload.
constexpr char magic[8] = {'M', 'A', 'Y', 'A', 'C', 'O', 'O', 'K'};
constexpr uint32_t entry_format = 1;
constexpr size_t header_size = sizeof(magic) + sizeof(uint32_t) + sizeof(uint64_t) + 32;

std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    if (!input) return std::nullopt;
    auto bytes = std::vector<std::byte>{};
    std::transform(std::istreambuf_iterator<char>(input), {}, std::back_inserter(bytes), [](char c) { return std::byte(c); });
    if (input.bad()) return std::nullopt;
    return bytes;
}
} // namespace

Sha256Digest CookKey::digest() const {
    auto hasher = Sha256{};
    hasher.update(kind);
    hasher.update("\n" + std::to_string(version) + "\n" + sha256_text(source) + "\n");
    hasher.update(settings);
    return hasher.finish();
}

CookCache::CookCache(std::filesystem::path folder) : m_folder(std::move(folder)) {}

std::filesystem::path CookCache::entry_path(const CookKey& key) const {
    const auto name = sha256_text(key.digest());
    return m_folder / name.substr(0, 2) / (name + "." + key.kind);
}

std::vector<std::byte> wrap_cooked(std::span<const std::byte> payload) {
    auto out = std::vector<std::byte>(header_size + payload.size());
    const auto size = uint64_t(payload.size());
    const auto digest = sha256(payload);
    auto* at = out.data();
    std::memcpy(at, magic, sizeof(magic));
    std::memcpy(at += sizeof(magic), &entry_format, sizeof(entry_format));
    std::memcpy(at += sizeof(entry_format), &size, sizeof(size));
    std::memcpy(at += sizeof(size), digest.data(), digest.size());
    if (!payload.empty()) std::memcpy(at + digest.size(), payload.data(), payload.size());
    return out;
}

std::optional<std::vector<std::byte>> unwrap_cooked(std::span<const std::byte> bytes) {
    if (bytes.size() < header_size || std::memcmp(bytes.data(), magic, sizeof(magic)) != 0) return std::nullopt;
    uint32_t format = 0;
    uint64_t size = 0;
    auto stored = Sha256Digest{};
    std::memcpy(&format, bytes.data() + sizeof(magic), sizeof(format));
    std::memcpy(&size, bytes.data() + sizeof(magic) + sizeof(format), sizeof(size));
    std::memcpy(stored.data(), bytes.data() + sizeof(magic) + sizeof(format) + sizeof(size), stored.size());
    const auto payload = bytes.subspan(header_size);
    if (format != entry_format || size != payload.size() || sha256(payload) != stored) return std::nullopt;
    return std::vector<std::byte>(payload.begin(), payload.end());
}

std::optional<std::vector<std::byte>> CookCache::read(const CookKey& key) {
    auto bytes = read_file(entry_path(key));
    if (!bytes) {
        ++m_stats.misses;
        return std::nullopt;
    }
    auto payload = unwrap_cooked(*bytes);
    if (!payload) {
        ++m_stats.damaged;
        ++m_stats.misses;
        return std::nullopt;
    }
    ++m_stats.hits;
    return payload;
}

void CookCache::write(const CookKey& key, std::span<const std::byte> payload) {
    const auto path = entry_path(key);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (!error && !std::filesystem::exists(m_folder / ".gitignore", error))
        replace_file(m_folder / ".gitignore", "# Maya's cook cache: cooked from the project's sources, never committed.\n*\n", "cache");
    const auto entry = wrap_cooked(payload);
    if (error || !replace_file(path, std::string(reinterpret_cast<const char*>(entry.data()), entry.size()), "cache entry").empty()) {
        ++m_stats.failures;
        return;
    }
    ++m_stats.writes;
}

Sha256Digest CookCache::source_digest(const std::filesystem::path& file, std::span<const std::byte> bytes) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    const auto digest = sha256(bytes);
    if (error) return digest;
    const auto entry = Remembered{time, bytes.size(), digest};
    const auto found = std::ranges::find(m_sources, file, &std::pair<std::filesystem::path, Remembered>::first);
    if (found != m_sources.end()) found->second = entry;
    else m_sources.emplace_back(file, entry);
    return digest;
}

std::optional<Sha256Digest> CookCache::source_digest(const std::filesystem::path& file) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    const auto size = error ? 0 : std::filesystem::file_size(file, error);
    if (error) return std::nullopt;
    const auto found = std::ranges::find(m_sources, file, &std::pair<std::filesystem::path, Remembered>::first);
    if (found != m_sources.end() && found->second.time == time && found->second.size == size) return found->second.digest;
    const auto bytes = read_file(file);
    if (!bytes) return std::nullopt;
    return source_digest(file, *bytes);
}

namespace {
/// An entry's digest, from its path: "<2 hex>/<64 hex>.<kind>" under the folder; empty for anything else.
std::string entry_name(const std::filesystem::path& path) {
    const auto stem = path.stem().string();
    const auto hex = stem.size() == 64 && std::ranges::all_of(stem, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
    if (!hex || path.extension().empty() || path.parent_path().filename() != stem.substr(0, 2)) return {};
    return stem;
}
} // namespace

CookCache::Usage CookCache::usage() const {
    auto usage = Usage{};
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(m_folder, error); !error && it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (!it->is_regular_file(error) || entry_name(it->path()).empty()) continue;
        ++usage.entries;
        usage.bytes += it->file_size(error);
    }
    return usage;
}

CookCache::Pruning CookCache::prune(std::span<const CookKey> reachable, bool dry_run) {
    auto keep = std::unordered_set<std::string>{};
    for (const auto& key : reachable) keep.insert(sha256_text(key.digest()));
    auto pruning = Pruning{};
    auto doomed = std::vector<std::pair<std::filesystem::path, uint64_t>>{};
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(m_folder, error); !error && it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const auto name = entry_name(it->path());
        if (name.empty()) continue;
        const auto size = it->file_size(error);
        if (keep.contains(name)) {
            ++pruning.kept;
            pruning.kept_bytes += size;
        } else {
            doomed.emplace_back(it->path(), size);
        }
    }
    for (const auto& [path, size] : doomed) { // after the walk: removing during it would disturb the iterator
        if (dry_run || (std::filesystem::remove(path, error) && !error)) {
            ++pruning.removed;
            pruning.removed_bytes += size;
        } else {
            pruning.errors.push_back(path.string() + ": " + (error ? error.message() : "not removed"));
        }
        error.clear();
    }
    return pruning;
}

std::filesystem::path cook_cache_folder(const Project& project) { return project.file.parent_path() / ".maya" / "cache"; }
} // namespace maya
