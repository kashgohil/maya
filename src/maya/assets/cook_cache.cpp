#include "maya/assets/cook_cache.hpp"
#include "maya/assets/project.hpp"
#include "maya/core/file_replace.hpp"
#include <algorithm>
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

std::optional<std::vector<std::byte>> CookCache::read(const CookKey& key) {
    auto bytes = read_file(entry_path(key));
    if (!bytes) {
        ++m_stats.misses;
        return std::nullopt;
    }
    uint32_t format = 0;
    uint64_t size = 0;
    auto stored = Sha256Digest{};
    if (bytes->size() >= header_size) {
        std::memcpy(&format, bytes->data() + sizeof(magic), sizeof(format));
        std::memcpy(&size, bytes->data() + sizeof(magic) + sizeof(format), sizeof(size));
        std::memcpy(stored.data(), bytes->data() + sizeof(magic) + sizeof(format) + sizeof(size), stored.size());
    }
    const auto payload = bytes->size() >= header_size ? std::span(*bytes).subspan(header_size) : std::span<const std::byte>{};
    if (bytes->size() < header_size || std::memcmp(bytes->data(), magic, sizeof(magic)) != 0 || format != entry_format ||
        size != payload.size() || sha256(payload) != stored) {
        ++m_stats.damaged;
        ++m_stats.misses;
        return std::nullopt;
    }
    ++m_stats.hits;
    return std::vector<std::byte>(payload.begin(), payload.end());
}

void CookCache::write(const CookKey& key, std::span<const std::byte> payload) {
    const auto path = entry_path(key);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (!error && !std::filesystem::exists(m_folder / ".gitignore", error))
        replace_file(m_folder / ".gitignore", "# Maya's cook cache: cooked from the project's sources, never committed.\n*\n", "cache");
    auto text = std::string(magic, sizeof(magic));
    const auto size = uint64_t(payload.size());
    const auto digest = sha256(payload);
    text.append(reinterpret_cast<const char*>(&entry_format), sizeof(entry_format));
    text.append(reinterpret_cast<const char*>(&size), sizeof(size));
    text.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    text.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (error || !replace_file(path, text, "cache entry").empty()) {
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

std::filesystem::path cook_cache_folder(const Project& project) { return project.file.parent_path() / ".maya" / "cache"; }
} // namespace maya
