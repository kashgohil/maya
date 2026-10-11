#include "maya/assets/generator_file.hpp"
#include "maya/core/file_replace.hpp"
#include "maya/properties/schema.hpp"
#include <algorithm>
#include <charconv>
#include <istream>
#include <sstream>
#include <vector>

namespace maya {
namespace {
std::string number(float value) {
    char text[32];
    const auto end = std::to_chars(text, text + sizeof(text), value).ptr; // shortest exact round trip
    return {text, end};
}
template<class T> bool parse(std::string_view text, T& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool parse_hex(std::string_view text, uint64_t& value) {
    if (text.empty() || text.size() > 16) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
std::string hex(uint64_t value) {
    char text[17];
    return {text, std::to_chars(text, text + sizeof(text), value, 16).ptr};
}
/// An asset reference: two hexadecimal ID words, or "none".
template<class T> std::optional<PropertyValue> reference(const std::vector<std::string>& words) {
    if (words.size() == 1 && words[0] == "none") return AssetRef<T>{};
    auto id = AssetId{};
    if (words.size() != 2 || !parse_hex(words[0], id.high) || !parse_hex(words[1], id.low) || !id.valid()) return std::nullopt;
    return AssetRef<T>{id};
}
std::string encode(const PropertyDescriptor& property, const PropertyValue& value) {
    return std::visit([&]<class T>(const T& typed) -> std::string {
        if constexpr (std::same_as<T, float>) return number(typed);
        else if constexpr (std::same_as<T, int32_t>) return std::to_string(typed);
        else if constexpr (std::same_as<T, bool>) return typed ? "true" : "false";
        else if constexpr (std::same_as<T, AssetRef<GeneratorAsset>> || std::same_as<T, AssetRef<MeshAsset>> ||
                           std::same_as<T, AssetRef<MaterialAsset>>)
            return typed.valid() ? hex(typed.id.high) + ' ' + hex(typed.id.low) : "none";
        else if constexpr (std::same_as<T, ChoiceValue>)
            return std::string(std::ranges::find(property.choices, typed.value, &EnumOption::value)->name);
        else return {}; // generators hold no other kinds of value
    }, value);
}
std::optional<PropertyValue> decode(const PropertyDescriptor& property, const std::vector<std::string>& words) {
    switch (property.type) {
    case PropertyType::generator_ref: return reference<GeneratorAsset>(words);
    case PropertyType::mesh_ref: return reference<MeshAsset>(words);
    case PropertyType::material_ref: return reference<MaterialAsset>(words);
    default: break;
    }
    if (words.size() != 1) return std::nullopt;
    switch (property.type) {
    case PropertyType::boolean:
        if (words[0] == "true" || words[0] == "false") return words[0] == "true";
        return std::nullopt;
    case PropertyType::scalar:
        if (float value = 0; parse(words[0], value)) return value;
        return std::nullopt;
    case PropertyType::integer:
        if (int32_t value = 0; parse(words[0], value)) return value;
        return std::nullopt;
    case PropertyType::choice:
        for (const auto& choice : property.choices) if (choice.name == words[0]) return ChoiceValue{choice.value};
        return std::nullopt;
    default: return std::nullopt;
    }
}
const char* expectation(PropertyType type) {
    switch (type) {
    case PropertyType::scalar: return "one finite number";
    case PropertyType::integer: return "one whole number";
    case PropertyType::choice: return "one of its choices";
    case PropertyType::boolean: return "true or false";
    case PropertyType::generator_ref: case PropertyType::mesh_ref: case PropertyType::material_ref:
        return "'none' or two hexadecimal asset ID words";
    default: return "a value";
    }
}
} // namespace

uint32_t current_algorithm(GeneratorKind kind) noexcept {
    switch (kind) {
    case GeneratorKind::heightfield_noise: return heightfield_noise_version;
    case GeneratorKind::scatter: return scatter_version;
    }
    return 0;
}

GeneratorFileResult read_generator_file(std::istream& input) {
    auto result = GeneratorFileResult{};
    const auto fail = [&](size_t line, std::string why) {
        result = {};
        result.error = "line " + std::to_string(line) + ": " + std::move(why);
        return result;
    };
    auto text = std::string{};
    if (!std::getline(input, text)) return fail(1, "Expected 'maya-generator 1'");
    {
        auto stream = std::istringstream(text);
        auto magic = std::string{}, extra = std::string{};
        auto version = 0u;
        if (!(stream >> magic >> version) || magic != "maya-generator") return fail(1, "Expected 'maya-generator 1'");
        if (version != generator_format_version)
            return fail(1, "Generator file version " + std::to_string(version) + " is not supported; this build reads version 1");
        if (stream >> extra) return fail(1, "Expected only 'maya-generator 1' on the first line");
    }
    auto generator = GeneratorAsset{};
    auto algorithm = std::optional<uint32_t>{};
    auto seen = std::vector<PropertyId>{};
    auto edits = std::vector<PropertyEdit>{};
    auto lines = std::vector<size_t>{}; // of each edit
    for (size_t line = 2; std::getline(input, text); ++line) {
        auto stream = std::istringstream(text);
        auto key = std::string{};
        if (!(stream >> key) || key.starts_with('#')) continue;
        auto values = std::vector<std::string>{};
        for (std::string word; stream >> word;) values.push_back(word);
        if (key == "algorithm") {
            if (algorithm) return fail(line, "'algorithm' appears more than once");
            if (uint32_t value = 0; values.size() == 1 && parse(values[0], value) && value > 0) algorithm = value;
            else return fail(line, "'algorithm' needs a whole number from 1");
            continue;
        }
        const auto* property = generator_property(key);
        if (!property) return fail(line, "Unknown generator property '" + key + "'");
        if (std::ranges::find(seen, property->id) != seen.end()) return fail(line, "'" + key + "' appears more than once");
        seen.push_back(property->id);
        const auto value = decode(*property, values);
        if (!value) return fail(line, "'" + key + "' needs " + expectation(property->type));
        edits.push_back({property->id, *value});
        lines.push_back(line);
    }
    if (input.bad()) return fail(0, "I/O failure while reading the generator");
    // Values are checked together, since a region's corners depend on each other; a failure names the line
    // of the property it is about.
    if (const auto edited = edit_properties(generator, edits); !edited) {
        const auto at = std::ranges::find(edits, edited.property, &PropertyEdit::property);
        const auto* property = generator_property(edited.property);
        return fail(at == edits.end() ? 0 : lines[size_t(at - edits.begin())],
                    (property ? "'" + std::string(property->name) + "': " : std::string{}) + std::string(edited.message));
    }
    // Each property belongs to a kind: one of another kind's is a mistake, not something to ignore.
    const auto kind_name = std::string(std::ranges::find(generator_property("kind")->choices, uint32_t(generator.kind), &EnumOption::value)->name);
    for (size_t i = 0; i < edits.size(); ++i)
        if (!generator_property_applies(edits[i].property, generator.kind))
            return fail(lines[i], "'" + std::string(generator_property(edits[i].property)->name) + "' is not a property of " + kind_name + " generators");
    if (!algorithm) return fail(0, "The generator names no 'algorithm' version");
    if (*algorithm != current_algorithm(generator.kind))
        return fail(0, "Algorithm version " + std::to_string(*algorithm) + " of " +
                           std::string(std::ranges::find(generator_property("kind")->choices, uint32_t(generator.kind), &EnumOption::value)->name) +
                           " is not one this build generates (it generates version " + std::to_string(current_algorithm(generator.kind)) + ")");
    generator.version = *algorithm;
    result.generator = generator;
    return result;
}

std::string write_generator_file(const GeneratorAsset& generator) {
    auto output = "maya-generator " + std::to_string(generator_format_version) + "\nalgorithm " + std::to_string(generator.version) + '\n';
    for (const auto& property : generator_properties(generator.kind)) // its kind's alone
        output += std::string(property.name) + ' ' + encode(property, *read_property(generator, property.id)) + '\n';
    return output;
}

std::string save_generator_file(const std::filesystem::path& path, const GeneratorAsset& generator) {
    auto failed = replace_file(path, write_generator_file(generator), "generator");
    return failed.empty() ? failed : path.string() + ": " + failed;
}
} // namespace maya
