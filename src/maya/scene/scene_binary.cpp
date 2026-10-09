#include "maya/scene/scene_binary.hpp"
#include "scene_detail.hpp"
#include <cstring>
#include <optional>
#include <type_traits>

namespace maya {
namespace {
constexpr char magic[8] = {'m', 'a', 'y', 'a', 'c', 'e', 'l', '1'};

class Writer {
public:
    std::string bytes;
    template<class T> void raw(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto at = bytes.size();
        bytes.resize(at + sizeof(T));
        std::memcpy(bytes.data() + at, &value, sizeof(T));
    }
    void text(std::string_view value) {
        raw(uint32_t(value.size()));
        bytes.append(value);
    }
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : m_bytes(bytes) {}
    template<class T> bool raw(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (m_bytes.size() - m_at < sizeof(T)) return false;
        std::memcpy(&value, m_bytes.data() + m_at, sizeof(T));
        m_at += sizeof(T);
        return true;
    }
    bool text(std::string& value) {
        auto size = uint32_t{};
        if (!raw(size) || m_bytes.size() - m_at < size) return false;
        value.assign(m_bytes.substr(m_at, size));
        m_at += size;
        return true;
    }
    bool done() const noexcept { return m_at == m_bytes.size(); }

private:
    std::string_view m_bytes;
    size_t m_at = 0;
};

void write_value(Writer& out, const PropertyValue& value) {
    out.raw(uint8_t(value.index()));
    std::visit([&]<class T>(const T& typed) {
        if constexpr (std::same_as<T, std::string>) out.text(typed);
        else if constexpr (std::same_as<T, bool>) out.raw(uint8_t(typed));
        else if constexpr (std::same_as<T, float> || std::same_as<T, int32_t> || std::same_as<T, uint32_t>) out.raw(typed);
        else if constexpr (std::same_as<T, math::Vec3>) out.raw(std::array{typed.x, typed.y, typed.z});
        else if constexpr (std::same_as<T, math::DVec3>) out.raw(std::array{typed.x, typed.y, typed.z});
        else if constexpr (std::same_as<T, math::Vec2>) out.raw(std::array{typed.x, typed.y});
        else if constexpr (std::same_as<T, math::Quat>) out.raw(std::array{typed.x, typed.y, typed.z, typed.w});
        else if constexpr (std::same_as<T, ChoiceValue>) out.raw(typed.value);
        else if constexpr (std::same_as<T, std::vector<ScriptValue>>) {
            out.raw(uint32_t(typed.size()));
            for (const auto& script : typed) {
                out.text(script.name);
                out.raw(uint8_t(script.type));
                out.raw(uint8_t(script.data.index()));
                std::visit([&]<class D>(const D& data) {
                    if constexpr (std::same_as<D, std::string>) out.text(data);
                    else if constexpr (std::same_as<D, bool>) out.raw(uint8_t(data));
                    else if constexpr (std::same_as<D, math::Vec3>) out.raw(std::array{data.x, data.y, data.z});
                    else if constexpr (std::same_as<D, EntityId>) out.raw(std::array{data.high, data.low});
                    else out.raw(data);
                }, script.data);
            }
        } else {
            out.raw(std::array{typed.id.high, typed.id.low}); // an asset reference
        }
    }, value);
}

/// The alternative `index` of a variant, default-constructed, or nullopt when out of range.
template<class Variant, size_t I = 0> std::optional<Variant> alternative(size_t index) {
    if constexpr (I == std::variant_size_v<Variant>) return std::nullopt;
    else return index == I ? std::optional<Variant>(std::in_place_index<I>) : alternative<Variant, I + 1>(index);
}

bool read_value(Reader& in, PropertyValue& value) {
    auto index = uint8_t{};
    if (!in.raw(index)) return false;
    auto made = alternative<PropertyValue>(index);
    if (!made) return false;
    value = std::move(*made);
    return std::visit([&]<class T>(T& typed) -> bool {
        if constexpr (std::same_as<T, std::string>) return in.text(typed);
        else if constexpr (std::same_as<T, bool>) {
            auto byte = uint8_t{};
            if (!in.raw(byte) || byte > 1) return false;
            typed = byte != 0;
            return true;
        } else if constexpr (std::same_as<T, float> || std::same_as<T, int32_t> || std::same_as<T, uint32_t>) return in.raw(typed);
        else if constexpr (std::same_as<T, math::Vec3>) {
            auto v = std::array<float, 3>{};
            if (!in.raw(v)) return false;
            typed = {v[0], v[1], v[2]};
            return true;
        } else if constexpr (std::same_as<T, math::DVec3>) {
            auto v = std::array<double, 3>{};
            if (!in.raw(v)) return false;
            typed = {v[0], v[1], v[2]};
            return true;
        } else if constexpr (std::same_as<T, math::Vec2>) {
            auto v = std::array<float, 2>{};
            if (!in.raw(v)) return false;
            typed = {v[0], v[1]};
            return true;
        } else if constexpr (std::same_as<T, math::Quat>) {
            auto v = std::array<float, 4>{};
            if (!in.raw(v)) return false;
            typed = {v[0], v[1], v[2], v[3]};
            return true;
        } else if constexpr (std::same_as<T, ChoiceValue>) return in.raw(typed.value);
        else if constexpr (std::same_as<T, std::vector<ScriptValue>>) {
            auto count = uint32_t{};
            if (!in.raw(count) || count > 4096) return false;
            typed.resize(count);
            for (auto& script : typed) {
                auto type = uint8_t{}, data = uint8_t{};
                if (!in.text(script.name) || !in.raw(type) || type > uint8_t(ScriptValueType::entity) || !in.raw(data)) return false;
                script.type = ScriptValueType(type);
                auto made_data = alternative<ScriptValueData>(data);
                if (!made_data) return false;
                script.data = std::move(*made_data);
                const auto ok = std::visit([&]<class D>(D& field) -> bool {
                    if constexpr (std::same_as<D, std::string>) return in.text(field);
                    else if constexpr (std::same_as<D, bool>) {
                        auto byte = uint8_t{};
                        if (!in.raw(byte) || byte > 1) return false;
                        field = byte != 0;
                        return true;
                    } else if constexpr (std::same_as<D, math::Vec3>) {
                        auto v = std::array<float, 3>{};
                        if (!in.raw(v)) return false;
                        field = {v[0], v[1], v[2]};
                        return true;
                    } else if constexpr (std::same_as<D, EntityId>) {
                        auto v = std::array<uint64_t, 2>{};
                        if (!in.raw(v)) return false;
                        field = {v[0], v[1]};
                        return true;
                    } else return in.raw(field);
                }, script.data);
                if (!ok) return false;
            }
            return true;
        } else {
            auto v = std::array<uint64_t, 2>{};
            if (!in.raw(v)) return false;
            typed = T{{v[0], v[1]}};
            return true;
        }
    }, value);
}

SceneDocumentResult refuse(std::string message) {
    auto result = SceneDocumentResult{};
    result.diagnostics.push_back({SceneError::malformed, "Cooked scene: " + std::move(message)});
    return result;
}
} // namespace

uint64_t scene_schema_fingerprint() {
    static const auto fingerprint = [] {
        auto hash = uint64_t{14695981039346656037ull};
        const auto fold = [&](const void* data, size_t size) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
        };
        for (const auto& schema : component_schemas()) {
            fold(schema.name.data(), schema.name.size());
            fold(&schema.version, sizeof schema.version);
            for (const auto& property : schema.properties) {
                fold(&property.id, sizeof property.id);
                const auto type = uint32_t(property.type);
                fold(&type, sizeof type);
                const auto index = uint32_t(property.default_value.index());
                fold(&index, sizeof index);
            }
        }
        return hash;
    }();
    return fingerprint;
}

std::string encode_scene_binary(const SceneDocument& document) {
    auto out = Writer{};
    out.bytes.append(magic, sizeof magic);
    out.raw(scene_schema_fingerprint());
    out.raw(uint32_t(document.entities.size()));
    for (const auto& entity : document.entities) {
        out.raw(std::array{entity.id.high, entity.id.low});
        out.raw(uint8_t(entity.parent.has_value()));
        if (entity.parent) out.raw(std::array{entity.parent->high, entity.parent->low});
        out.raw(uint32_t(entity.components.size()));
        for (const auto& value : entity.components) {
            const auto& schema = *component_schema(component_id(value));
            out.raw(uint32_t(schema.id));
            out.raw(uint32_t(schema.properties.size()));
            for (const auto& property : schema.properties) {
                out.raw(property.id);
                write_value(out, *read_property(value, property.id));
            }
        }
    }
    return std::move(out.bytes);
}

SceneDocumentResult decode_scene_binary(std::string_view bytes, const PropertyValidationContext& context) {
    if (bytes.size() < sizeof magic || std::memcmp(bytes.data(), magic, sizeof magic) != 0) return refuse("not a cooked scene");
    auto in = Reader(bytes.substr(sizeof magic));
    auto fingerprint = uint64_t{};
    if (!in.raw(fingerprint)) return refuse("truncated header");
    if (fingerprint != scene_schema_fingerprint()) return refuse("cooked by a build with other component schemas");
    auto count = uint32_t{};
    if (!in.raw(count) || count > bytes.size()) return refuse("truncated header");
    auto document = SceneDocument{};
    document.entities.reserve(count);
    auto edits = std::vector<PropertyEdit>{};
    for (uint32_t e = 0; e < count; ++e) {
        auto& entity = document.entities.emplace_back();
        auto id = std::array<uint64_t, 2>{};
        auto has_parent = uint8_t{};
        if (!in.raw(id) || !in.raw(has_parent) || has_parent > 1) return refuse("truncated entity");
        entity.id = {id[0], id[1]};
        if (has_parent) {
            if (!in.raw(id)) return refuse("truncated entity");
            entity.parent = EntityId{id[0], id[1]};
        }
        auto components = uint32_t{};
        if (!in.raw(components) || components > 64) return refuse("bad component count");
        for (uint32_t c = 0; c < components; ++c) {
            auto component = uint32_t{}, properties = uint32_t{};
            if (!in.raw(component) || !in.raw(properties) || properties > 256) return refuse("truncated component");
            auto value = default_component(ComponentId(component));
            if (!value) return refuse("unknown component " + std::to_string(component));
            edits.clear();
            for (uint32_t p = 0; p < properties; ++p) {
                auto edit = PropertyEdit{};
                if (!in.raw(edit.property) || !read_value(in, edit.value)) return refuse("truncated property");
                edits.push_back(std::move(edit));
            }
            if (const auto result = edit_properties(*value, edits, context); !result)
                return refuse("entity " + detail::id_text(entity.id) + ": " + std::string(result.message));
            entity.components.push_back(std::move(*value));
        }
    }
    if (!in.done()) return refuse("trailing bytes");
    auto result = SceneDocumentResult{};
    result.diagnostics = validate_scene(document, context);
    if (result.diagnostics.empty()) result.document = std::move(document);
    return result;
}

} // namespace maya
