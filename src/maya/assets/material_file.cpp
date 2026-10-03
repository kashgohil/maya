#include "maya/assets/material_file.hpp"
#include "maya/core/file_replace.hpp"
#include "maya/properties/schema.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <istream>
#include <sstream>
#include <vector>

namespace maya {
namespace {
std::string hex(uint64_t value) {
    char text[17];
    const auto end = std::to_chars(text, text + sizeof(text), value, 16).ptr;
    return {text, end};
}
std::string number(float value) {
    char text[32];
    const auto end = std::to_chars(text, text + sizeof(text), value).ptr; // shortest exact round trip
    return {text, end};
}
bool parse(std::string_view text, float& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool parse_hex(std::string_view text, uint64_t& value) {
    if (text.empty() || text.size() > 16) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
std::string encode(const PropertyDescriptor& property, const PropertyValue& value) {
    return std::visit([&]<class T>(const T& typed) -> std::string {
        if constexpr (std::same_as<T, bool>) return typed ? "true" : "false";
        else if constexpr (std::same_as<T, float>) return number(typed);
        else if constexpr (std::same_as<T, math::Vec3>) return number(typed.x) + ' ' + number(typed.y) + ' ' + number(typed.z);
        else if constexpr (std::same_as<T, ChoiceValue>)
            return std::string(std::ranges::find(property.choices, typed.value, &EnumOption::value)->name);
        else if constexpr (std::same_as<T, AssetRef<TextureAsset>>)
            return typed.valid() ? hex(typed.id.high) + ' ' + hex(typed.id.low) : "none";
        else return {}; // materials hold no other kinds of value
    }, value);
}
std::optional<PropertyValue> decode(const PropertyDescriptor& property, const std::vector<std::string>& words) {
    switch (property.type) {
    case PropertyType::boolean:
        if (words.size() == 1 && (words[0] == "true" || words[0] == "false")) return words[0] == "true";
        return std::nullopt;
    case PropertyType::scalar:
        if (float value = 0; words.size() == 1 && parse(words[0], value)) return value;
        return std::nullopt;
    case PropertyType::vector3:
        if (math::Vec3 v; words.size() == 3 && parse(words[0], v.x) && parse(words[1], v.y) && parse(words[2], v.z)) return v;
        return std::nullopt;
    case PropertyType::choice:
        if (words.size() != 1) return std::nullopt;
        for (const auto& choice : property.choices) if (choice.name == words[0]) return ChoiceValue{choice.value};
        return std::nullopt;
    case PropertyType::texture_ref: {
        if (words.size() == 1 && words[0] == "none") return AssetRef<TextureAsset>{};
        auto id = AssetId{};
        if (words.size() != 2 || !parse_hex(words[0], id.high) || !parse_hex(words[1], id.low) || !id.valid()) return std::nullopt;
        return AssetRef<TextureAsset>{id};
    }
    default: return std::nullopt;
    }
}
const char* expectation(PropertyType type) {
    switch (type) {
    case PropertyType::boolean: return "true or false";
    case PropertyType::scalar: return "one finite number";
    case PropertyType::vector3: return "three finite numbers";
    case PropertyType::choice: return "one of its choices";
    case PropertyType::texture_ref: return "'none' or two hexadecimal texture ID words";
    default: return "a value";
    }
}
/// Version 1: exactly base_color RGBA, metallic, and roughness, each a factor in [0, 1].
MaterialFileResult read_version_1(std::istream& input) {
    auto result = MaterialFileResult{};
    auto& material = result.material;
    auto& color = material.base_color;
    std::string color_key, metallic_key, roughness_key, extra;
    if (!(input >> color_key >> color.x >> color.y >> color.z >> color.w >> metallic_key >> material.metallic >>
          roughness_key >> material.roughness) ||
        color_key != "base_color" || metallic_key != "metallic" || roughness_key != "roughness" || input >> extra || input.bad()) {
        result.error = "Expected maya-material 1, base_color RGBA, metallic, roughness";
        return result;
    }
    for (const float value : {color.x, color.y, color.z, color.w, material.metallic, material.roughness})
        if (!std::isfinite(value) || value < 0 || value > 1) {
            result.error = "Material factors must be finite in [0,1]";
            return result;
        }
    result.version = 1;
    return result;
}
} // namespace

MaterialFileResult read_material_file(std::istream& input) {
    auto result = MaterialFileResult{};
    const auto fail = [&](size_t line, std::string why) {
        result = {};
        result.error = "line " + std::to_string(line) + ": " + std::move(why);
        return result;
    };
    // Version 1 is whitespace-separated words, as its reader always took them; version 2 is lines.
    auto magic = std::string{};
    auto version = 0u;
    if (!(input >> magic >> version) || magic != "maya-material") return fail(1, "Expected 'maya-material 2'");
    if (version == 1) return read_version_1(input);
    if (version != material_format_version)
        return fail(1, "Material file version " + std::to_string(version) + " is not supported; this build reads versions 1 and 2");
    auto rest = std::string{};
    std::getline(input, rest);
    if (rest.find_first_not_of(" \t\r") != std::string::npos) return fail(1, "Expected only 'maya-material 2' on the first line");

    auto material = MaterialAsset{};
    auto seen = std::vector<PropertyId>{};
    auto text = std::string{};
    for (size_t line = 2; std::getline(input, text); ++line) {
        auto stream = std::istringstream(text);
        auto key = std::string{};
        if (!(stream >> key) || key.starts_with('#')) continue;
        const auto* property = material_property(key);
        if (!property) return fail(line, "Unknown material property '" + key + "'");
        if (std::ranges::find(seen, property->id) != seen.end()) return fail(line, "'" + key + "' appears more than once");
        seen.push_back(property->id);
        auto values = std::vector<std::string>{};
        for (std::string word; stream >> word;) values.push_back(word);
        const auto value = decode(*property, values);
        if (!value) return fail(line, "'" + key + "' needs " + expectation(property->type));
        const auto edit = PropertyEdit{property->id, *value};
        // Only the form of texture references is checked here; extraction reports missing textures.
        const auto any_texture = PropertyValidationContext{[](AssetId, ReferenceKind) { return ReferenceStatus::valid; }};
        if (const auto edited = edit_properties(material, std::span(&edit, 1), any_texture); !edited)
            return fail(line, "'" + key + "': " + std::string(edited.message));
    }
    if (input.bad()) return fail(0, "I/O failure while reading the material");
    result.material = material;
    result.version = version;
    return result;
}

std::string write_material_file(const MaterialAsset& material) {
    auto output = "maya-material " + std::to_string(material_format_version) + '\n';
    for (const auto& property : material_properties())
        output += std::string(property.name) + ' ' + encode(property, *read_property(material, property.id)) + '\n';
    return output;
}

std::string save_material_file(const std::filesystem::path& path, const MaterialAsset& material) {
    auto failed = replace_file(path, write_material_file(material), "material");
    return failed.empty() ? failed : path.string() + ": " + failed;
}
} // namespace maya
