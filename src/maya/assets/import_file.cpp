#include "maya/assets/import_file.hpp"
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <istream>
#include <set>
#include <sstream>

namespace maya {
namespace {
bool parse_hex(const std::string& word, uint64_t& value) {
    if (word.empty() || word.size() > 16) return false;
    const auto parsed = std::from_chars(word.data(), word.data() + word.size(), value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == word.data() + word.size();
}
std::string hex(uint64_t value) {
    auto text = std::ostringstream{};
    text << std::hex << value;
    return text.str();
}
std::string quoted(const std::string& text) {
    auto out = std::ostringstream{};
    out << std::quoted(text);
    return out.str();
}
} // namespace

ImportFileResult read_import_file(std::istream& input) {
    auto result = ImportFileResult{};
    const auto fail = [&](size_t line, std::string why) {
        result = {};
        result.error = "line " + std::to_string(line) + ": " + std::move(why);
        return result;
    };
    auto text = std::string{};
    if (!std::getline(input, text) || text.find_first_not_of(" \t\r") == std::string::npos) return fail(1, "Expected 'maya-import 1'");
    {
        auto stream = std::istringstream(text);
        auto magic = std::string{}, extra = std::string{};
        auto version = 0u;
        if (!(stream >> magic >> version) || magic != "maya-import" || stream >> extra) return fail(1, "Expected 'maya-import 1'");
        if (version != import_format_version)
            return fail(1, "Import file version " + std::to_string(version) + " is not supported; this build reads version 1");
    }
    auto& file = result.file;
    bool scene = false, entries = false;
    auto settings_seen = std::set<std::string>{};
    const auto inside = [](const std::string& text) {
        const auto path = std::filesystem::path(text).lexically_normal();
        return !text.empty() && !path.is_absolute() && !path.has_root_name() && *path.begin() != "..";
    };
    auto ids = std::array<std::set<std::pair<uint64_t, uint64_t>>, 6>{};
    for (size_t line = 2; std::getline(input, text); ++line) {
        auto stream = std::istringstream(text);
        auto key = std::string{};
        if (!(stream >> key) || key.starts_with('#')) continue;
        auto extra = std::string{};
        if (key == "compression" || key == "mips" || key == "scale" || key == "up" || key == "lights" || key == "cameras") {
            if (entries) return fail(line, "'" + key + "' must come before the entries");
            if (!settings_seen.insert(key).second) return fail(line, "'" + key + "' appears more than once");
            auto value = std::string{};
            if (!(stream >> value) || stream >> extra) return fail(line, "'" + key + "' needs one value");
            const auto toggle = [&](bool& setting) {
                if (value != "on" && value != "off") return false;
                setting = value == "on";
                return true;
            };
            auto& settings = file.settings;
            if (key == "compression") {
                if (value == "astc") settings.compression = TextureCompression::astc;
                else if (value == "rgba8") settings.compression = TextureCompression::rgba8;
                else return fail(line, "'compression' must be astc or rgba8");
            } else if (key == "scale") {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), settings.scale);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(settings.scale) ||
                    settings.scale < min_import_scale || settings.scale > max_import_scale)
                    return fail(line, "'scale' must be a number from 1e-06 to 1e+06");
            } else if (key == "up") {
                if (value == "y") settings.up = ImportUp::y;
                else if (value == "z") settings.up = ImportUp::z;
                else return fail(line, "'up' must be y or z");
            } else if (!toggle(key == "mips" ? settings.mips : key == "lights" ? settings.lights : settings.cameras)) {
                return fail(line, "'" + key + "' must be on or off");
            }
            continue;
        }
        if (key == "scene") {
            if (entries) return fail(line, "'scene' must come before the entries");
            if (scene) return fail(line, "'scene' appears more than once");
            scene = true;
            auto name = std::string{}, hash_word = std::string{};
            if (!(stream >> std::quoted(name) >> hash_word) || !parse_hex(hash_word, file.scene_written) || stream >> extra)
                return fail(line, "'scene' needs a quoted file and a hash");
            if (!inside(name)) return fail(line, "the scene file '" + name + "' must stay at or below the import file's folder");
            file.scene = std::filesystem::path(name).lexically_normal();
            continue;
        }
        if (key == "file") {
            entries = true;
            auto name = std::string{};
            if (!(stream >> std::quoted(name)) || stream >> extra) return fail(line, "'file' needs a quoted file");
            if (!inside(name)) return fail(line, "the file '" + name + "' must stay at or below the import file's folder");
            file.files.push_back(std::filesystem::path(name).lexically_normal());
            continue;
        }
        const auto kind = key == "mesh" ? 0 : key == "texture" ? 1 : key == "material" ? 2 : key == "entity" ? 3
                        : key == "skin" ? 4 : key == "animation" ? 5 : -1;
        if (kind < 0) return fail(line, "Unknown key '" + key + "'");
        entries = true;
        auto high_word = std::string{}, low_word = std::string{}, first = std::string{}, identity = std::string{};
        uint64_t high = 0, low = 0;
        if (!(stream >> high_word >> low_word) || !parse_hex(high_word, high) || !parse_hex(low_word, low) || (high == 0 && low == 0))
            return fail(line, "'" + key + "' needs a nonzero ID as two hexadecimal words");
        if (!ids[size_t(kind)].emplace(high, low).second) return fail(line, "the " + key + " ID " + high_word + " " + low_word + " appears more than once");
        if (kind == 3) {
            if (!(stream >> std::quoted(identity)) || stream >> extra) return fail(line, "'entity' needs an ID and a quoted node path");
            file.entities.push_back({EntityId{high, low}, identity});
        } else if (kind == 2) {
            auto hash_word = std::string{};
            uint64_t written = 0;
            if (!(stream >> std::quoted(first) >> std::quoted(identity) >> hash_word) || !parse_hex(hash_word, written) || stream >> extra)
                return fail(line, "'material' needs an ID, a quoted file, a quoted identity, and a hash");
            if (!inside(first)) return fail(line, "the material file '" + first + "' must stay at or below the import file's folder");
            file.materials.push_back({AssetId{high, low}, std::filesystem::path(first).lexically_normal(), identity, written});
        } else {
            if (!(stream >> std::quoted(first) >> std::quoted(identity)) || stream >> extra)
                return fail(line, "'" + key + "' needs an ID, a quoted part, and a quoted identity");
            if (!first.starts_with(key + "/")) return fail(line, "a " + key + "'s part starts with '" + key + "/'");
            auto& list = kind == 0 ? file.meshes : kind == 1 ? file.textures : kind == 4 ? file.skins : file.animations;
            list.push_back({AssetId{high, low}, first, identity});
        }
    }
    if (input.bad()) return fail(0, "I/O failure while reading the import file");
    return result;
}

std::string write_import_file(const ImportFile& file) {
    const auto& settings = file.settings;
    const auto on = [](bool value) { return value ? "on" : "off"; };
    char scale[32];
    const auto end = std::to_chars(scale, scale + sizeof(scale), settings.scale).ptr; // shortest exact round trip
    auto text = "maya-import " + std::to_string(import_format_version) + "\ncompression " + texture_compression_name(settings.compression) +
                "\nmips " + on(settings.mips) + "\nscale " + std::string(scale, end) + "\nup " + (settings.up == ImportUp::y ? "y" : "z") +
                "\nlights " + on(settings.lights) + "\ncameras " + on(settings.cameras) + "\n";
    if (!file.scene.empty()) text += "scene " + quoted(file.scene.generic_string()) + ' ' + hex(file.scene_written) + '\n';
    const auto id = [](const auto& value) { return hex(value.high) + ' ' + hex(value.low); };
    for (const auto& mesh : file.meshes) text += "mesh " + id(mesh.id) + ' ' + quoted(mesh.part) + ' ' + quoted(mesh.identity) + '\n';
    for (const auto& texture : file.textures)
        text += "texture " + id(texture.id) + ' ' + quoted(texture.part) + ' ' + quoted(texture.identity) + '\n';
    for (const auto& material : file.materials)
        text += "material " + id(material.id) + ' ' + quoted(material.file.generic_string()) + ' ' + quoted(material.identity) + ' ' +
                hex(material.written) + '\n';
    for (const auto& skin : file.skins) text += "skin " + id(skin.id) + ' ' + quoted(skin.part) + ' ' + quoted(skin.identity) + '\n';
    for (const auto& clip : file.animations)
        text += "animation " + id(clip.id) + ' ' + quoted(clip.part) + ' ' + quoted(clip.identity) + '\n';
    for (const auto& entity : file.entities) text += "entity " + id(entity.id) + ' ' + quoted(entity.identity) + '\n';
    for (const auto& named : file.files) text += "file " + quoted(named.generic_string()) + '\n';
    return text;
}

std::filesystem::path import_file_path(const std::filesystem::path& source) {
    auto path = source;
    path += ".import";
    return path;
}

uint64_t import_text_hash(std::string_view text) noexcept {
    auto hash = uint64_t{0xcbf29ce484222325};
    for (const auto c : text) {
        hash ^= uint8_t(c);
        hash *= 0x100000001b3;
    }
    return hash;
}
} // namespace maya
