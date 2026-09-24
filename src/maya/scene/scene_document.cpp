#include "scene_detail.hpp"
#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace maya {
namespace detail {
bool valid_utf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const auto length = lead < 0x80 ? 1u : (lead & 0xe0) == 0xc0 ? 2u
            : (lead & 0xf0) == 0xe0 ? 3u : (lead & 0xf8) == 0xf0 ? 4u : 0u;
        if (length == 0 || i + length > text.size()) return false;
        uint32_t code = length == 1 ? lead : lead & (0x7f >> length);
        for (size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        // Reject overlong forms, surrogates, and values beyond Unicode.
        constexpr uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (code < minimum[length] || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        i += length;
    }
    return true;
}
namespace {
const char* world_error_text(WorldError error) {
    switch (error) {
    case WorldError::none: return "none";
    case WorldError::busy: return "busy";
    case WorldError::wrong_world: return "wrong world";
    case WorldError::invalid_entity: return "invalid entity";
    case WorldError::invalid_id: return "invalid ID";
    case WorldError::duplicate_id: return "duplicate ID";
    case WorldError::invalid_pending_entity: return "invalid pending entity";
    case WorldError::component_exists: return "component exists";
    case WorldError::component_missing: return "component missing";
    case WorldError::capacity_exhausted: return "capacity exhausted";
    case WorldError::invalid_transform: return "invalid transform";
    case WorldError::hierarchy_cycle: return "hierarchy cycle";
    case WorldError::hierarchy_in_use: return "hierarchy in use";
    case WorldError::unrepresentable_transform: return "unrepresentable transform";
    case WorldError::invalid_policy: return "invalid policy";
    }
    return "unknown";
}
std::string at_line(size_t line, const std::string& text) {
    return line ? "line " + std::to_string(line) + ": " + text : text;
}
bool has_transform(const SceneEntity& entity) {
    return std::ranges::any_of(entity.components, [](const auto& value) {
        return component_id(value) == ComponentId::transform;
    });
}
} // namespace

void SceneReport::add(SceneError code, std::string message, size_t line, EntityId entity) {
    if (m_output.size() + 1 < max_scene_diagnostics) {
        m_output.push_back({code, std::move(message), line, entity});
    } else if (m_output.size() + 1 == max_scene_diagnostics) {
        m_output.push_back({code, "Further problems omitted; fix the reported problems and retry", line, entity});
    }
}

std::string id_text(uint64_t high, uint64_t low) {
    char text[40];
    std::snprintf(text, sizeof(text), "%llx:%llx", static_cast<unsigned long long>(high),
                  static_cast<unsigned long long>(low));
    return text;
}

std::string entity_location(const SceneEntity& entity) {
    auto text = "entity " + id_text(entity.id);
    for (const auto& value : entity.components)
        if (const auto name = std::get_if<NameComponent>(&value); name && !name->value.empty())
            return text + " ('" + name->value + "')";
    return text;
}

void report_property_error(SceneReport& report, const PropertyResult& result, ComponentId component,
                           const std::optional<PropertyValue>& value, const std::string& location,
                           size_t line, EntityId entity) {
    const auto schema = component_schema(component);
    const auto property = property_schema(component, result.property);
    auto field = location + ", " + std::string(schema ? schema->name : "unknown component");
    if (property) field += "." + std::string(property->name);
    auto asset = std::string("the referenced asset");
    if (value) {
        if (const auto mesh = std::get_if<AssetRef<MeshAsset>>(&*value)) asset = "asset " + id_text(mesh->id);
        if (const auto material = std::get_if<AssetRef<MaterialAsset>>(&*value)) asset = "asset " + id_text(material->id);
    }
    switch (result.error) {
    case PropertyError::missing_reference:
        report.add(SceneError::missing_asset, at_line(line, field + ": " + asset +
            " is not in the project catalog; restore its catalog entry or clear the reference"), line, entity);
        return;
    case PropertyError::wrong_reference_type:
        report.add(SceneError::wrong_asset_type, at_line(line, field + ": " + asset +
            " is registered as a different asset kind; reference a " +
            (property && property->type == PropertyType::mesh_ref ? "mesh" : "material") +
            " asset or clear the reference"), line, entity);
        return;
    case PropertyError::validation_context_required:
        report.add(SceneError::validation_context_required, at_line(line, field +
            ": asset references need the project catalog; load with asset_property_context(registry)"),
            line, entity);
        return;
    default:
        report.add(SceneError::invalid_value, at_line(line, field + ": " + std::string(result.message)),
                   line, entity);
    }
}

SceneDiagnostics validate_scene(SceneDocument& document, const PropertyValidationContext& context,
                                const SceneLines* lines) {
    SceneDiagnostics diagnostics;
    SceneReport report(diagnostics);
    const auto count = document.entities.size();
    const auto entity_line = [&](size_t index) { return lines ? lines->entity[index] : size_t{0}; };
    const auto field_line = [&](size_t index, ComponentId component, PropertyId property) {
        if (!lines) return size_t{0};
        const auto found = lines->fields.find({index, component, property});
        return found == lines->fields.end() ? entity_line(index) : found->second;
    };

    std::unordered_map<EntityId, size_t, PersistentIdHash> index;
    for (size_t i = 0; i < count; ++i) {
        auto& entity = document.entities[i];
        const auto location = entity_location(entity);
        if (!entity.id.valid()) {
            report.add(SceneError::invalid_value, at_line(entity_line(i),
                "An entity uses the reserved zero ID; assign it a generated EntityId"), entity_line(i));
        } else if (const auto [first, added] = index.emplace(entity.id, i); !added) {
            const auto previous = entity_line(first->second);
            report.add(SceneError::duplicate_entity, at_line(entity_line(i), location +
                " is defined more than once" + (previous ? " (first on line " + std::to_string(previous) + ")" : "") +
                "; every entity in a scene needs a unique ID"), entity_line(i), entity.id);
        }
        auto seen = std::vector<ComponentId>{};
        for (auto& value : entity.components) {
            const auto component = component_id(value);
            if (std::ranges::find(seen, component) != seen.end()) {
                report.add(SceneError::duplicate_component, at_line(field_line(i, component, 0), location +
                    " has more than one " + std::string(component_schema(component)->name) +
                    " component; keep one"), field_line(i, component, 0), entity.id);
                continue;
            }
            seen.push_back(component);
            if (!lines || !lines->components_validated) {
                if (const auto result = validate_component(value, context); !result) {
                    const auto rejected = result.property ? read_property(value, result.property) : std::nullopt;
                    const auto line = field_line(i, component, result.property);
                    report_property_error(report, result, component, rejected, location, line, entity.id);
                }
            }
            if (const auto name = std::get_if<NameComponent>(&value); name && !valid_utf8(name->value)) {
                const auto line = field_line(i, component, 1);
                report.add(SceneError::invalid_value, at_line(line, location +
                    ", maya.name.value: text is not valid UTF-8; scene files store UTF-8 text"), line, entity.id);
            }
        }
    }

    constexpr auto none = static_cast<size_t>(-1);
    auto parents = std::vector<size_t>(count, none);
    for (size_t i = 0; i < count; ++i) {
        const auto& entity = document.entities[i];
        if (!entity.parent) continue;
        const auto line = lines && lines->parent[i] ? lines->parent[i] : entity_line(i);
        const auto location = entity_location(entity);
        const auto found = index.find(*entity.parent);
        if (*entity.parent == entity.id) {
            report.add(SceneError::invalid_hierarchy, at_line(line, location +
                " is its own parent; remove the parent or choose another entity"), line, entity.id);
        } else if (found == index.end()) {
            report.add(SceneError::invalid_hierarchy, at_line(line, location + " names parent " +
                id_text(*entity.parent) + ", which is not in this scene; restore the parent or remove the link"),
                line, entity.id);
        } else {
            parents[i] = found->second;
            if (!has_transform(entity))
                report.add(SceneError::invalid_hierarchy, at_line(line, location +
                    " has a parent but no maya.transform component; add one or remove the parent"), line, entity.id);
            if (!has_transform(document.entities[found->second]))
                report.add(SceneError::invalid_hierarchy, at_line(line, location + " names parent " +
                    entity_location(document.entities[found->second]) +
                    ", which has no maya.transform component; add one or remove the link"), line, entity.id);
        }
    }
    // Iterative cycle detection: 1 = on the current parent chain, 2 = known acyclic.
    auto state = std::vector<uint8_t>(count, 0);
    auto chain = std::vector<size_t>{};
    for (size_t start = 0; start < count; ++start) {
        chain.clear();
        for (auto current = start; current != none;) {
            if (state[current] == 2) break;
            if (state[current] == 1) {
                const auto& entity = document.entities[current];
                const auto line = lines && lines->parent[current] ? lines->parent[current] : entity_line(current);
                report.add(SceneError::invalid_hierarchy, at_line(line, entity_location(entity) +
                    " is its own ancestor through its parent links; break the cycle"), line, entity.id);
                break;
            }
            state[current] = 1;
            chain.push_back(current);
            current = parents[current];
        }
        for (const auto visited : chain) state[visited] = 2;
    }
    return diagnostics;
}
} // namespace detail

SceneDocument capture_scene(const World& world) {
    auto roots = std::vector<std::pair<EntityId, EntityHandle>>{};
    world.for_each_entity([&](EntityHandle entity) {
        if (!world.parent(entity)) roots.emplace_back(*world.persistent_id(entity), entity);
    });
    std::ranges::sort(roots, {}, &std::pair<EntityId, EntityHandle>::first);

    auto document = SceneDocument{};
    document.entities.reserve(world.size());
    auto pending = std::vector<EntityHandle>{};
    for (auto root = roots.rbegin(); root != roots.rend(); ++root) pending.push_back(root->second);
    while (!pending.empty()) {
        const auto entity = pending.back();
        pending.pop_back();
        auto& saved = document.entities.emplace_back();
        saved.id = *world.persistent_id(entity);
        if (const auto parent = world.parent(entity)) saved.parent = world.persistent_id(*parent);
        for (const auto& schema : component_schemas())
            if (auto value = read_component(world, entity, schema.id)) saved.components.push_back(std::move(*value));
        const auto children = world.children(entity);
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return document;
}

SceneDiagnostics validate_scene(SceneDocument& document, const PropertyValidationContext& context) {
    return detail::validate_scene(document, context, nullptr);
}

SceneWorldResult instantiate_scene(SceneDocument document, const PropertyValidationContext& context) {
    if (auto diagnostics = validate_scene(document, context); !diagnostics.empty())
        return {nullptr, std::move(diagnostics)};
    auto world = std::make_unique<World>();
    auto commands = world->commands();
    auto pending = std::vector<PendingEntity>{};
    auto index = std::unordered_map<EntityId, size_t, PersistentIdHash>{};
    pending.reserve(document.entities.size());
    for (const auto& entity : document.entities) {
        index.emplace(entity.id, pending.size());
        pending.push_back(commands.create(entity.id));
    }
    for (size_t i = 0; i < document.entities.size(); ++i)
        for (auto& value : document.entities[i].components)
            std::visit([&](auto& component) { commands.add(pending[i], std::move(component)); }, value);
    // Reparenting inserts at the head of the child list, so link in reverse to keep document order.
    for (auto i = document.entities.size(); i-- > 0;)
        if (const auto& parent = document.entities[i].parent)
            commands.reparent(pending[i], pending[index.at(*parent)], ReparentPolicy::keep_local);
    const auto result = world->commit(commands);
    if (!result)
        return {nullptr, {{SceneError::world_rejected, "The World rejected the scene at command " +
            std::to_string(result.command_index) + " (" + detail::world_error_text(result.error) +
            "); the active scene was not changed"}}};
    return {std::move(world), {}};
}
} // namespace maya
