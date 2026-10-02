#include "maya/assets/texture_data.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <istream>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace maya {
namespace {
const char* filter_name(Filter filter) { return filter == Filter::nearest ? "nearest" : "linear"; }
const char* mip_filter_name(MipFilter filter) {
    return filter == MipFilter::none ? "none" : filter == MipFilter::nearest ? "nearest" : "linear";
}
const char* address_name(AddressMode mode) {
    return mode == AddressMode::repeat ? "repeat" : mode == AddressMode::clamp_to_edge ? "clamp" : "mirror";
}
template<class T, size_t N>
bool lookup(std::string_view word, const std::array<std::pair<std::string_view, T>, N>& names, T& value) {
    for (const auto& [name, candidate] : names)
        if (word == name) {
            value = candidate;
            return true;
        }
    return false;
}
constexpr auto roles = std::array{std::pair{std::string_view("color"), TextureRole::color},
                                  std::pair{std::string_view("data"), TextureRole::data},
                                  std::pair{std::string_view("normal"), TextureRole::normal}};
constexpr auto compressions = std::array{std::pair{std::string_view("astc"), TextureCompression::astc},
                                         std::pair{std::string_view("rgba8"), TextureCompression::rgba8}};
constexpr auto filters = std::array{std::pair{std::string_view("nearest"), Filter::nearest},
                                    std::pair{std::string_view("linear"), Filter::linear}};
constexpr auto mip_filters = std::array{std::pair{std::string_view("none"), MipFilter::none},
                                        std::pair{std::string_view("nearest"), MipFilter::nearest},
                                        std::pair{std::string_view("linear"), MipFilter::linear}};
constexpr auto addresses = std::array{std::pair{std::string_view("repeat"), AddressMode::repeat},
                                      std::pair{std::string_view("clamp"), AddressMode::clamp_to_edge},
                                      std::pair{std::string_view("mirror"), AddressMode::mirror_repeat}};
constexpr auto keys = std::array<std::string_view, 8>{"source", "usage", "compression", "mips",
                                                      "filter", "mip_filter", "anisotropy", "address"};
} // namespace

const char* texture_role_name(TextureRole role) noexcept {
    return role == TextureRole::color ? "color" : role == TextureRole::data ? "data" : "normal";
}
const char* texture_compression_name(TextureCompression compression) noexcept {
    return compression == TextureCompression::astc ? "astc" : "rgba8";
}
Format texture_format(TextureRole role, TextureCompression compression) noexcept {
    if (compression == TextureCompression::rgba8) return role == TextureRole::color ? Format::rgba8_srgb : Format::rgba8_unorm;
    switch (role) {
    case TextureRole::color: return Format::astc_6x6_srgb;
    case TextureRole::data: return Format::astc_6x6_unorm;
    case TextureRole::normal: return Format::astc_4x4_unorm;
    }
    return Format::undefined;
}

TextureSettingsResult read_texture_settings(std::istream& input) {
    auto result = TextureSettingsResult{};
    auto& settings = result.settings;
    auto line = std::string{};
    auto number = 0;
    const auto fail = [&](const std::string& message) {
        result.error = "line " + std::to_string(number) + ": " + message;
        return result;
    };
    // The header is the first line that is not blank.
    while (std::getline(input, line)) {
        ++number;
        if (line.find_first_not_of(" \t\r") != std::string::npos) break;
        line.clear();
    }
    {
        auto words = std::istringstream(line);
        auto magic = std::string{}, extra = std::string{};
        auto version = 0;
        if (number == 0) {
            result.error = "the file is empty; expected the header 'maya-texture 1'";
            return result;
        }
        if (!(words >> magic >> version) || magic != "maya-texture" || words >> extra)
            return fail("expected the header 'maya-texture 1'");
        if (version != 1) return fail("unsupported texture file version " + std::to_string(version) + "; this build reads version 1");
    }
    auto seen = std::map<std::string, int>{};
    auto mip_filter_line = 0;
    while (std::getline(input, line)) {
        ++number;
        auto words = std::istringstream(line);
        auto key = std::string{};
        if (!(words >> key)) continue; // blank line
        if (std::ranges::find(keys, key) == keys.end()) return fail("unknown setting '" + key + "'");
        if (const auto [it, added] = seen.emplace(key, number); !added)
            return fail("'" + key + "' is set again (first on line " + std::to_string(it->second) + ")");
        const auto word = [&](std::string& value) { return static_cast<bool>(words >> value); };
        auto a = std::string{}, b = std::string{};
        if (key == "source") {
            auto path = std::string{};
            if (!(words >> std::quoted(path)) || path.empty()) return fail("source needs a quoted, nonempty path");
            const auto source = std::filesystem::path(path);
            if (source.is_absolute() || source.has_root_name()) return fail("source must be relative to the texture file's folder");
            for (const auto& part : source.lexically_normal())
                if (part == "..") return fail("source must stay in the texture file's folder or below it");
            settings.source = source.lexically_normal();
        } else if (key == "usage") {
            if (!word(a) || !lookup(a, roles, settings.role)) return fail("usage must be color, data, or normal");
        } else if (key == "compression") {
            if (!word(a) || !lookup(a, compressions, settings.compression)) return fail("compression must be astc or rgba8");
        } else if (key == "mips") {
            if (!word(a) || (a != "on" && a != "off")) return fail("mips must be on or off");
            settings.mips = a == "on";
        } else if (key == "filter") {
            if (!word(a) || !word(b) || !lookup(a, filters, settings.sampler.min_filter) || !lookup(b, filters, settings.sampler.mag_filter))
                return fail("filter needs two of nearest or linear: minification, then magnification");
        } else if (key == "mip_filter") {
            if (!word(a) || !lookup(a, mip_filters, settings.sampler.mip_filter)) return fail("mip_filter must be none, nearest, or linear");
            mip_filter_line = number;
        } else if (key == "anisotropy") {
            auto value = 0u;
            const auto parsed = word(a) ? std::from_chars(a.data(), a.data() + a.size(), value) : std::from_chars_result{nullptr, std::errc::invalid_argument};
            if (parsed.ec != std::errc{} || parsed.ptr != a.data() + a.size() || value < 1 || value > 16)
                return fail("anisotropy must be a whole number from 1 (off) to 16");
            settings.sampler.max_anisotropy = value;
        } else if (key == "address") {
            if (!word(a) || !word(b) || !lookup(a, addresses, settings.sampler.address_u) || !lookup(b, addresses, settings.sampler.address_v))
                return fail("address needs two of repeat, clamp, or mirror: u, then v");
        }
        if (auto extra = std::string{}; words >> extra) return fail("unexpected '" + extra + "' after " + key);
    }
    if (input.bad()) return fail("the file could not be read");
    for (const auto key : keys)
        if (!seen.contains(std::string(key))) {
            result.error = "'" + std::string(key) + "' is missing; a texture file states every setting";
            return result;
        }
    if (!settings.mips && settings.sampler.mip_filter != MipFilter::none) {
        number = mip_filter_line;
        return fail("mip_filter must be none when mips is off");
    }
    return result;
}

void write_texture_settings(std::ostream& output, const TextureSettings& settings) {
    auto text = std::ostringstream{};
    const auto& sampler = settings.sampler;
    text << "maya-texture 1\n"
         << "source " << std::quoted(settings.source.generic_string()) << '\n'
         << "usage " << texture_role_name(settings.role) << '\n'
         << "compression " << texture_compression_name(settings.compression) << '\n'
         << "mips " << (settings.mips ? "on" : "off") << '\n'
         << "filter " << filter_name(sampler.min_filter) << ' ' << filter_name(sampler.mag_filter) << '\n'
         << "mip_filter " << mip_filter_name(sampler.mip_filter) << '\n'
         << "anisotropy " << sampler.max_anisotropy << '\n'
         << "address " << address_name(sampler.address_u) << ' ' << address_name(sampler.address_v) << '\n';
    output << text.str();
    if (!output) throw std::runtime_error("Cannot write texture settings");
}
} // namespace maya
