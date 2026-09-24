#include "scene_detail.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace maya {
namespace {
using detail::SceneLines;

// Format version 1 keywords. Property names must never use them.
constexpr std::string_view header_keyword = "maya-scene";

struct Token {
    std::string text;
    bool quoted = false;
};

std::string hex(uint64_t value) {
    char text[17];
    const auto end = std::to_chars(text, text + sizeof(text), value, 16).ptr;
    return {text, end};
}
void append_float(std::string& output, float value) {
    char text[32];
    const auto end = std::to_chars(text, text + sizeof(text), value).ptr; // shortest exact round trip
    output.append(text, end);
}
void append_quoted(std::string& output, std::string_view text) {
    constexpr auto digits = "0123456789abcdef";
    output += '"';
    for (const auto raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                output += "\\x";
                output += digits[c >> 4];
                output += digits[c & 0xf];
            } else {
                output += raw;
            }
        }
    }
    output += '"';
}
void append_value(std::string& output, const PropertyValue& value, const PropertyDescriptor& property) {
    std::visit([&]<class T>(const T& typed) {
        if constexpr (std::same_as<T, std::string>) append_quoted(output, typed);
        else if constexpr (std::same_as<T, bool>) output += typed ? "true" : "false";
        else if constexpr (std::same_as<T, float>) append_float(output, typed);
        else if constexpr (std::same_as<T, math::Vec3>) {
            for (const auto component : {typed.x, typed.y, typed.z}) {
                append_float(output, component);
                output += ' ';
            }
            output.pop_back();
        } else if constexpr (std::same_as<T, math::Quat>) {
            for (const auto component : {typed.x, typed.y, typed.z, typed.w}) {
                append_float(output, component);
                output += ' ';
            }
            output.pop_back();
        } else if constexpr (std::same_as<T, LightKind>) {
            const auto choice = std::ranges::find(property.choices, typed, &EnumOption::value);
            output += choice->name;
        } else {
            output += typed.valid() ? hex(typed.id.high) + ' ' + hex(typed.id.low) : std::string("none");
        }
    }, value);
}

std::string encode(const SceneDocument& document) {
    auto output = std::string(header_keyword) + ' ' + std::to_string(scene_format_version) + '\n';
    auto order = std::vector<const ComponentValue*>{};
    for (const auto& entity : document.entities) {
        output += "\nentity " + hex(entity.id.high) + ' ' + hex(entity.id.low) + '\n';
        if (entity.parent) output += "  parent " + hex(entity.parent->high) + ' ' + hex(entity.parent->low) + '\n';
        order.clear();
        for (const auto& value : entity.components) order.push_back(&value);
        std::ranges::sort(order, {}, [](const ComponentValue* value) { return component_id(*value); });
        for (const auto value : order) {
            const auto& schema = *component_schema(component_id(*value));
            output += "  component " + std::string(schema.name) + ' ' + std::to_string(schema.version) + '\n';
            for (const auto& property : schema.properties) {
                output += "    " + std::string(property.name) + ' ';
                append_value(output, *read_property(*value, property.id), property);
                output += '\n';
            }
        }
        output += "end\n";
    }
    return output;
}

bool control(unsigned char c) { return (c < 0x20 && c != '\t') || c == 0x7f; }
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/// Splits on spaces/tabs; quoted strings are single tokens. Blank and '#' comment lines are empty.
bool tokenize(std::string_view line, std::vector<Token>& tokens, std::string& error) {
    tokens.clear();
    size_t i = 0;
    const auto space = [&] { return line[i] == ' ' || line[i] == '\t'; };
    while (true) {
        while (i < line.size() && space()) ++i;
        if (i == line.size() || (tokens.empty() && line[i] == '#')) return true;
        auto& token = tokens.emplace_back();
        if (line[i] != '"') {
            for (; i < line.size() && !space(); ++i) {
                if (line[i] == '"') return error = "unexpected quote inside a word", false;
                if (control(static_cast<unsigned char>(line[i]))) return error = "control character in text", false;
                token.text += line[i];
            }
            continue;
        }
        token.quoted = true;
        for (++i;; ++i) {
            if (i == line.size()) return error = "unterminated quoted string", false;
            const auto c = line[i];
            if (c == '"') break;
            if (control(static_cast<unsigned char>(c)))
                return error = "control character in quoted string; use an escape such as \\n", false;
            if (c != '\\') {
                token.text += c;
                continue;
            }
            if (++i == line.size()) return error = "unterminated escape sequence", false;
            switch (line[i]) {
            case '"': token.text += '"'; break;
            case '\\': token.text += '\\'; break;
            case 'n': token.text += '\n'; break;
            case 'r': token.text += '\r'; break;
            case 't': token.text += '\t'; break;
            case 'x': {
                const auto high = i + 1 < line.size() ? hex_digit(line[i + 1]) : -1;
                const auto low = i + 2 < line.size() ? hex_digit(line[i + 2]) : -1;
                const auto value = high * 16 + low;
                if (high < 0 || low < 0 || !control(static_cast<unsigned char>(value)))
                    return error = "\\x escapes are only for control characters (00-1f, 7f)", false;
                token.text += static_cast<char>(value);
                i += 2;
                break;
            }
            default: return error = std::string("unsupported escape \\") + line[i], false;
            }
        }
        if (++i < line.size() && !space()) return error = "expected a space after a quoted string", false;
    }
}

bool parse_word(const Token& token, uint64_t& value) {
    const auto& text = token.text;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return !token.quoted && !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool parse_decimal(const Token& token, uint32_t& value) {
    const auto& text = token.text;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !token.quoted && !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool parse_float(const Token& token, float& value) {
    const auto& text = token.text;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    return !token.quoted && !text.empty() && parsed.ec == std::errc{} &&
        parsed.ptr == text.data() + text.size() && std::isfinite(value);
}
template<class Tag> bool parse_id(std::span<const Token> tokens, PersistentId<Tag>& id) {
    return tokens.size() == 2 && parse_word(tokens[0], id.high) && parse_word(tokens[1], id.low) && id.valid();
}

std::string expectation(const PropertyDescriptor& property) {
    switch (property.type) {
    case PropertyType::text: return "one quoted string";
    case PropertyType::boolean: return "true or false";
    case PropertyType::scalar: return "one finite number";
    case PropertyType::vector3: return "three finite numbers";
    case PropertyType::quaternion: return "four finite numbers (x y z w)";
    case PropertyType::light_kind: {
        auto names = std::string("one of");
        for (const auto& choice : property.choices) names += " " + std::string(choice.name);
        return names;
    }
    case PropertyType::mesh_ref:
    case PropertyType::material_ref: return "'none' or two hexadecimal asset ID words";
    }
    return "a value";
}
std::optional<PropertyValue> decode(const PropertyDescriptor& property, std::span<const Token> tokens) {
    const auto floats = [&](auto&... values) -> bool {
        if (tokens.size() != sizeof...(values)) return false;
        size_t i = 0;
        return (parse_float(tokens[i++], values) && ...);
    };
    const auto single_word = tokens.size() == 1 && !tokens[0].quoted;
    switch (property.type) {
    case PropertyType::text:
        if (tokens.size() == 1 && tokens[0].quoted) return tokens[0].text;
        return std::nullopt;
    case PropertyType::boolean:
        if (single_word && (tokens[0].text == "true" || tokens[0].text == "false")) return tokens[0].text == "true";
        return std::nullopt;
    case PropertyType::scalar:
        if (float value = 0; floats(value)) return value;
        return std::nullopt;
    case PropertyType::vector3:
        if (math::Vec3 value; floats(value.x, value.y, value.z)) return value;
        return std::nullopt;
    case PropertyType::quaternion:
        if (math::Quat value; floats(value.x, value.y, value.z, value.w)) return value;
        return std::nullopt;
    case PropertyType::light_kind:
        if (!single_word) return std::nullopt;
        for (const auto& choice : property.choices) if (choice.name == tokens[0].text) return choice.value;
        return std::nullopt;
    case PropertyType::mesh_ref:
    case PropertyType::material_ref: {
        auto id = AssetId{};
        if (!(single_word && tokens[0].text == "none") && !parse_id(tokens, id)) return std::nullopt;
        if (property.type == PropertyType::mesh_ref) return AssetRef<MeshAsset>{id};
        return AssetRef<MaterialAsset>{id};
    }
    }
    return std::nullopt;
}

std::string supported_components() {
    auto names = std::string{};
    for (const auto& schema : component_schemas()) names += (names.empty() ? "" : ", ") + std::string(schema.name);
    return names;
}

class Parser {
public:
    Parser(std::string_view text, const PropertyValidationContext& context)
        : m_text(text), m_context(context) {}

    SceneDocumentResult run() {
        if (!detail::valid_utf8(m_text)) {
            const auto line = invalid_utf8_line();
            return fail(SceneError::malformed, line, "text is not valid UTF-8; scene files are UTF-8 text");
        }
        for (size_t start = 0, next = 0; start <= m_text.size(); start = next) {
            const auto end = m_text.find('\n', start);
            next = end == std::string_view::npos ? m_text.size() + 1 : end + 1;
            auto line = m_text.substr(start, (end == std::string_view::npos ? m_text.size() : end) - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            ++m_line;
            if (std::string error; !tokenize(line, m_tokens, error))
                return fail(SceneError::malformed, m_line, error);
            if (!m_tokens.empty() && !statement()) return {{}, std::move(m_diagnostics)};
        }
        if (!m_header) return fail(SceneError::malformed, 0, "empty file; expected a 'maya-scene 1' header");
        if (m_in_entity)
            return fail(SceneError::malformed, m_lines.entity.back(),
                        "entity " + detail::id_text(m_document.entities.back().id) + " is missing its 'end' line");
        m_lines.components_validated = true;
        for (auto& diagnostic : detail::validate_scene(m_document, m_context, &m_lines))
            m_report.add(diagnostic.code, std::move(diagnostic.message), diagnostic.line, diagnostic.entity);
        if (!m_diagnostics.empty()) return {{}, std::move(m_diagnostics)};
        return {std::move(m_document), {}};
    }

private:
    struct RawProperty {
        const PropertyDescriptor* descriptor;
        std::vector<Token> tokens;
        size_t line;
    };

    SceneDocumentResult fail(SceneError code, size_t line, const std::string& message) {
        m_report.add(code, line ? "line " + std::to_string(line) + ": " + message : message, line);
        return {{}, std::move(m_diagnostics)};
    }
    bool stop(SceneError code, const std::string& message) {
        m_report.add(code, "line " + std::to_string(m_line) + ": " + message, m_line,
                      m_in_entity ? m_document.entities.back().id : EntityId{});
        return false;
    }
    size_t invalid_utf8_line() const {
        size_t line = 1, start = 0;
        for (size_t end; (end = m_text.find('\n', start)) != std::string_view::npos; start = end + 1, ++line)
            if (!detail::valid_utf8(m_text.substr(start, end - start))) return line;
        return line;
    }

    // Returns false after recording a fatal syntax/structure diagnostic.
    bool statement() {
        const auto& keyword = m_tokens.front();
        const auto arguments = std::span<const Token>(m_tokens).subspan(1);
        if (keyword.quoted) return stop(SceneError::malformed, "expected a keyword, found a quoted string");
        if (!m_header) return header(keyword, arguments);
        if (keyword.text == "entity") return entity(arguments);
        if (!m_in_entity)
            return stop(SceneError::malformed, "expected 'entity', found '" + keyword.text + "'");
        if (keyword.text == "end") return end_entity(arguments);
        if (keyword.text == "parent") return parent(arguments);
        if (keyword.text == "component") return component(arguments);
        if (!m_component)
            return stop(SceneError::malformed, "'" + keyword.text + "' appears before any 'component' line");
        if (std::ranges::any_of(m_properties, [&](const auto& raw) { return raw.descriptor->name == keyword.text; }))
            return stop(SceneError::duplicate_property, std::string(m_component->name) + "." + keyword.text +
                " is written more than once; keep one value");
        const auto descriptor = property_schema(m_component->id, keyword.text);
        if (!descriptor)
            return stop(SceneError::unknown_property, "unknown property '" + keyword.text + "' in " +
                std::string(m_component->name) + " version " + std::to_string(m_component->version) +
                "; remove it or open the scene with the Maya build that wrote it");
        m_properties.push_back({descriptor, {arguments.begin(), arguments.end()}, m_line});
        return true;
    }
    bool header(const Token& keyword, std::span<const Token> arguments) {
        uint32_t version = 0;
        if (keyword.text != header_keyword || arguments.size() != 1 || !parse_decimal(arguments[0], version) ||
            version == 0)
            return stop(SceneError::malformed, "expected a 'maya-scene 1' header; this is not a Maya scene file");
        if (version != scene_format_version)
            return stop(SceneError::unsupported_version, "scene format version " + std::to_string(version) +
                " is newer than this build supports (" + std::to_string(scene_format_version) +
                "); open it with a newer Maya build");
        m_header = true;
        return true;
    }
    bool entity(std::span<const Token> arguments) {
        if (m_in_entity)
            return stop(SceneError::malformed, "'entity' before the previous entity's 'end' line");
        auto id = EntityId{};
        if (!parse_id(arguments, id))
            return stop(SceneError::malformed, "'entity' needs two hexadecimal ID words, not both zero");
        m_document.entities.push_back({id, std::nullopt, {}});
        m_lines.entity.push_back(m_line);
        m_lines.parent.push_back(0);
        m_in_entity = true;
        return true;
    }
    bool parent(std::span<const Token> arguments) {
        auto& entity = m_document.entities.back();
        if (m_component || entity.parent)
            return stop(SceneError::malformed, "'parent' must appear once, before the entity's components");
        auto id = EntityId{};
        if (!parse_id(arguments, id))
            return stop(SceneError::malformed, "'parent' needs two hexadecimal entity ID words, not both zero");
        entity.parent = id;
        m_lines.parent.back() = m_line;
        return true;
    }
    bool component(std::span<const Token> arguments) {
        if (!finish_component()) return false;
        uint32_t version = 0;
        if (arguments.size() != 2 || arguments[0].quoted || !parse_decimal(arguments[1], version) || version == 0)
            return stop(SceneError::malformed, "'component' needs a stable component name and a version number");
        const auto schema = component_schema(arguments[0].text);
        if (!schema)
            return stop(SceneError::unknown_component, "unknown component '" + arguments[0].text +
                "'; this build supports " + supported_components() +
                ". The scene may come from a newer Maya build");
        const auto index = m_document.entities.size() - 1;
        if (m_lines.fields.contains({index, schema->id, 0}))
            return stop(SceneError::duplicate_component, "entity " + detail::id_text(m_document.entities.back().id) +
                " has more than one " + std::string(schema->name) + " component; keep one");
        if (version > schema->version)
            return stop(SceneError::unsupported_version, std::string(schema->name) + " version " +
                std::to_string(version) + " is newer than this build supports (" +
                std::to_string(schema->version) + "); open it with a newer Maya build");
        if (version < schema->version)
            // Initial schemas are version 1; a later version bump adds a keyed migration here.
            return stop(SceneError::unsupported_version, "no migration from " + std::string(schema->name) +
                " version " + std::to_string(version) + " to " + std::to_string(schema->version));
        m_lines.fields[{index, schema->id, 0}] = m_line;
        m_component = schema;
        m_component_line = m_line;
        return true;
    }
    bool end_entity(std::span<const Token> arguments) {
        if (!arguments.empty()) return stop(SceneError::malformed, "'end' takes no arguments");
        if (!finish_component()) return false;
        m_in_entity = false;
        return true;
    }
    // Decodes every raw property, then validates the complete component through the shared schema.
    bool finish_component() {
        if (!m_component) return true;
        const auto schema = std::exchange(m_component, nullptr);
        const auto index = m_document.entities.size() - 1;
        auto& entity = m_document.entities.back();
        auto edits = std::vector<PropertyEdit>{};
        for (auto& raw : m_properties) {
            auto value = decode(*raw.descriptor, raw.tokens);
            if (!value) {
                m_line = raw.line;
                return stop(SceneError::malformed, std::string(schema->name) + "." +
                    std::string(raw.descriptor->name) + " expects " + expectation(*raw.descriptor));
            }
            m_lines.fields[{index, schema->id, raw.descriptor->id}] = raw.line;
            edits.push_back({raw.descriptor->id, std::move(*value)});
        }
        m_properties.clear();
        for (const auto& property : schema->properties) {
            if (std::ranges::none_of(edits, [&](const auto& edit) { return edit.property == property.id; })) {
                m_line = m_component_line;
                return stop(SceneError::missing_property, std::string(schema->name) + " is missing '" +
                    std::string(property.name) + "'; version " + std::to_string(schema->version) +
                    " scene files store every property");
            }
        }
        auto value = *default_component(schema->id);
        if (const auto result = edit_properties(value, edits, m_context); !result) {
            // Semantic problems are collected so one load reports every bad value or missing asset.
            const auto rejected = std::ranges::find(edits, result.property, &PropertyEdit::property);
            const auto line = rejected != edits.end() ? m_lines.fields[{index, schema->id, result.property}]
                                                      : m_component_line;
            detail::report_property_error(m_report, result, schema->id,
                rejected != edits.end() ? std::optional(rejected->value) : std::nullopt,
                detail::entity_location(entity), line, entity.id);
        }
        entity.components.push_back(std::move(value));
        return true;
    }

    std::string_view m_text;
    const PropertyValidationContext& m_context;
    SceneDiagnostics m_diagnostics;
    detail::SceneReport m_report{m_diagnostics};
    SceneDocument m_document;
    SceneLines m_lines;
    std::vector<Token> m_tokens;
    std::vector<RawProperty> m_properties;
    const ComponentDescriptor* m_component = nullptr;
    size_t m_component_line = 0;
    size_t m_line = 0;
    bool m_header = false;
    bool m_in_entity = false;
};

void prefix(SceneDiagnostics& diagnostics, const std::filesystem::path& path) {
    for (auto& diagnostic : diagnostics) diagnostic.message = path.string() + ": " + diagnostic.message;
}
SceneDiagnostics io_failure(const std::filesystem::path& path, const std::string& message) {
    return {{SceneError::io_error, path.string() + ": " + message, 0, {}}};
}
std::string system_error(int code) { return std::strerror(code); }
} // namespace

SceneDocumentResult read_scene(std::string_view text, const PropertyValidationContext& context) {
    return Parser(text, context).run();
}

SceneDocumentResult read_scene(std::istream& input, const PropertyValidationContext& context) {
    const auto text = std::string(std::istreambuf_iterator<char>(input), {});
    if (input.bad()) return {{}, {{SceneError::io_error, "I/O failure while reading the scene", 0, {}}}};
    return read_scene(std::string_view(text), context);
}

SceneDiagnostics write_scene(std::ostream& output, SceneDocument document, const PropertyValidationContext& context) {
    if (auto diagnostics = validate_scene(document, context); !diagnostics.empty()) return diagnostics;
    const auto text = encode(document);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) return {{SceneError::io_error, "I/O failure while writing the scene", 0, {}}};
    return {};
}

SceneDocumentResult load_scene_file(const std::filesystem::path& path, const PropertyValidationContext& context) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (status.type() == std::filesystem::file_type::not_found)
        return {{}, io_failure(path, "scene file does not exist; check the path or restore the file")};
    if (error) return {{}, io_failure(path, "cannot inspect scene file: " + error.message())};
    if (!std::filesystem::is_regular_file(status))
        return {{}, io_failure(path, "not a regular file; choose a scene file")};
    auto input = std::ifstream(path, std::ios::binary);
    if (!input) return {{}, io_failure(path, "cannot open scene file for reading; check its permissions")};
    auto result = read_scene(input, context);
    prefix(result.diagnostics, path);
    return result;
}

SceneWorldResult open_scene_file(const std::filesystem::path& path, const PropertyValidationContext& context) {
    auto loaded = load_scene_file(path, context);
    if (!loaded) return {nullptr, std::move(loaded.diagnostics)};
    auto result = instantiate_scene(std::move(loaded.document), context);
    prefix(result.diagnostics, path);
    return result;
}

SceneDiagnostics save_scene_file(const std::filesystem::path& path, SceneDocument document,
                                 const PropertyValidationContext& context) {
    if (auto diagnostics = validate_scene(document, context); !diagnostics.empty()) {
        prefix(diagnostics, path);
        return diagnostics;
    }
    if (path.filename().empty() || path.filename() == "." || path.filename() == "..")
        return io_failure(path, "scene path has no file name");
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    const auto exists = status.type() != std::filesystem::file_type::not_found;
    if (error && exists) return io_failure(path, "cannot inspect the existing scene file: " + error.message());
    if (exists && !std::filesystem::is_regular_file(status))
        return io_failure(path, "exists and is not a regular file; choose another path");
    const auto text = encode(document);

    // A unique sibling keeps the final rename on one filesystem, so it replaces the file atomically.
    const auto directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const auto temporary = directory / ("." + path.filename().string() + ".tmp-" +
        hex(static_cast<uint64_t>(::getpid())) + "-" + hex(detail::next_lifetime_token()));
    const auto descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (descriptor < 0)
        return io_failure(path, "cannot create a temporary file in '" + directory.string() + "': " +
            system_error(errno) + ". The existing scene file was not changed");
    auto descriptor_open = true;
    const auto abandon = [&](const std::string& what, int code) {
        if (descriptor_open) ::close(descriptor);
        ::unlink(temporary.c_str());
        return io_failure(path, what + ": " + system_error(code) + ". The existing scene file was not changed");
    };
    for (size_t written = 0; written < text.size();) {
        const auto count = ::write(descriptor, text.data() + written, text.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return abandon("cannot write the scene", count < 0 ? errno : EIO);
        written += static_cast<size_t>(count);
    }
    if (exists) ::fchmod(descriptor, static_cast<mode_t>(status.permissions()) & 07777); // best effort
    // F_FULLFSYNC asks the drive to persist data; fall back where the filesystem does not support it.
    if (::fcntl(descriptor, F_FULLFSYNC) == -1 && ::fsync(descriptor) == -1)
        return abandon("cannot flush the scene to storage", errno);
    descriptor_open = false;
    if (::close(descriptor) != 0) return abandon("cannot finish writing the scene", errno);
    if (::rename(temporary.c_str(), path.c_str()) != 0)
        return abandon("cannot replace the scene file", errno);
    // Persist the directory entry. The new file is already in place, so failure here is not reported.
    if (const auto folder = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC); folder >= 0) {
        ::fsync(folder);
        ::close(folder);
    }
    return {};
}
} // namespace maya
