// Inspector: controls generated from the shared property schemas (#994). Every change goes through
// validation and SceneEditor, so it is undoable; a drag or a text entry is one undo step.
#include "editor_math.hpp"
#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include "maya/assets/property_context.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace maya::editor {
using namespace detail;
namespace {
constexpr auto degrees_per_radian = 180.0f / math::PI;

const char* component_icon(ComponentId id) {
    switch (id) {
    case ComponentId::transform: return icon::arrows_move;
    case ComponentId::mesh_renderer: return icon::cube;
    case ComponentId::camera: return icon::video_camera;
    case ComponentId::light: return icon::sun;
    case ComponentId::name: return icon::pencil;
    case ComponentId::spin: return icon::rotate;
    case ComponentId::fly_control: return icon::game_controller;
    case ComponentId::collider: return icon::bounding_box;
    case ComponentId::rigid_body: return icon::atom;
    case ComponentId::physics_settings: return icon::planet;
    }
    return icon::circle_dashed;
}

/// Whether a component's property applies to its current settings; others stay hidden (see their
/// descriptions): a light's range and cones, a collider's size for other shapes, a kinematic body's
/// initial velocities.
bool property_applies(const ComponentValue& value, std::string_view property) {
    if (const auto* light = std::get_if<LightComponent>(&value)) {
        if (property == "range") return light->kind != LightKind::directional;
        if (property == "inner_cone" || property == "outer_cone") return light->kind == LightKind::spot;
    } else if (const auto* collider = std::get_if<ColliderComponent>(&value)) {
        if (property == "half_extents") return collider->shape == ColliderShape::box;
        if (property == "radius") return collider->shape != ColliderShape::box;
        if (property == "half_height") return collider->shape == ColliderShape::capsule;
    } else if (const auto* body = std::get_if<RigidBodyComponent>(&value)) {
        if (property == "linear_velocity" || property == "angular_velocity") return body->motion == BodyMotion::dynamic;
    }
    return true;
}

/// "All groups", "None", one group's name, or how many groups a collision mask includes.
std::string mask_summary(uint32_t mask, const CollisionGroupNames& groups) {
    if ((mask & 0xFFFFu) == 0xFFFFu) return "All groups";
    if ((mask & 0xFFFFu) == 0) return "None";
    auto count = 0;
    auto last = size_t{0};
    for (size_t i = 0; i < collision_group_names; ++i)
        if (mask >> i & 1u) { ++count; last = i; }
    return count == 1 ? collision_group_label(groups, last) : std::to_string(count) + " groups";
}

/// True while the last drag field is in text-entry mode (command-click or double-click). Its value is
/// applied when entry ends, not on every keystroke, so a half-typed number never becomes an edit.
bool typing_into_last_item() { return ImGui::TempInputIsActive(ImGui::GetItemID()); }

/// Drag limits from a schema range; exclusive bounds step just inside.
void limits(const NumericRange& range, float scale, float& minimum, float& maximum) {
    minimum = range.minimum ? *range.minimum * scale : -FLT_MAX;
    maximum = range.maximum ? *range.maximum * scale : FLT_MAX;
    if (range.minimum && !range.minimum_inclusive) minimum = std::nextafter(minimum, FLT_MAX) + 1e-4f * scale;
    if (range.maximum && !range.maximum_inclusive) maximum = std::nextafter(maximum, -FLT_MAX) - 1e-4f * scale;
}

/// Three axis-labelled drag fields on one row. Returns true when any changed.
bool axis_fields(const char* id, float values[3], float speed, float minimum, float maximum, const char* format,
                 EditorLayout& layout, const std::string& key, bool& activated, bool& deactivated) {
    static constexpr ImU32 axis_colors[] = {theme::color::rgb(0xF2616B), theme::color::rgb(0x4ADE80), theme::color::rgb(0x7B8CFF)};
    static constexpr const char* axis_names[] = {"x", "y", "z"};
    auto changed = false;
    const auto spacing = 4.0f;
    const auto width = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
    ImGui::PushID(id);
    for (int axis = 0; axis < 3; ++axis) {
        if (axis) ImGui::SameLine(0.0f, spacing);
        ImGui::PushID(axis);
        ImGui::SetNextItemWidth(width);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::text);
        const auto moved = ImGui::DragFloat("##axis", &values[axis], speed, minimum, maximum, format, ImGuiSliderFlags_AlwaysClamp);
        changed |= moved && !typing_into_last_item();
        ImGui::PopStyleColor();
        activated |= ImGui::IsItemActivated();
        deactivated |= ImGui::IsItemDeactivated();
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        layout.inspector_fields.push_back({key + "." + axis_names[axis], min, max});
        // A short colored bar on the field's left edge marks the axis.
        ImGui::GetWindowDrawList()->AddRectFilled({min.x + 3.0f, min.y + 6.0f}, {min.x + 5.0f, max.y - 6.0f}, axis_colors[axis], 1.0f);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}
} // namespace

void EditorShell::track_edit(const std::string& label) {
    // A drag or text entry spans frames: group its edits so it is one undo step.
    if (ImGui::IsItemActivated() && !m_edit_group_open && m_scene) {
        m_scene->begin_group(label);
        m_edit_group_open = true;
    }
    if (ImGui::IsItemDeactivated() && m_edit_group_open && m_scene) {
        m_scene->end_group();
        m_edit_group_open = false;
    }
}

bool EditorShell::edit_property(EntityId id, const ComponentValue& value, PropertyId property, PropertyValue input) {
    auto edited = value;
    const auto edit = PropertyEdit{property, std::move(input)};
    const auto context = m_assets ? asset_property_context(*m_assets) : PropertyValidationContext{};
    if (const auto result = edit_properties(edited, std::span(&edit, 1), context); !result) {
        m_edit_error = std::string(result.message);
        m_edit_error_entity = id;
        return false;
    }
    const auto applied = m_scene->set_component(id, std::move(edited));
    if (!applied && applied.error != "Nothing to change") {
        m_edit_error = applied.error;
        m_edit_error_entity = id;
        return false;
    }
    m_edit_error.clear();
    return true;
}

void EditorShell::draw_component(EntityId id, const ComponentValue& value) {
    const auto component = component_id(value);
    const auto* schema = component_schema(component);
    const auto label = std::string(schema->label);
    ImGui::PushID(static_cast<int>(component));
    // Header: icon, label, and a remove button.
    ImGui::Dummy({0.0f, 4.0f});
    icon_text(component_icon(component), theme::color::muted);
    ImGui::PushFont(m_fonts.strong);
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopFont();
    if (component != ComponentId::name) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
        ImGui::PushStyleColor(ImGuiCol_Button, 0u);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.0f, 2.0f});
        if (ImGui::Button(icon::trash)) {
            const auto removed = m_scene->remove_component(id, component);
            if (!removed) {
                m_edit_error = removed.error;
                m_edit_error_entity = id;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove %s", label.c_str());
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    const auto group = "Edit " + label;
    if (theme::begin_properties("properties")) {
        for (const auto& property : schema->properties) {
            const auto current = read_property(value, property.id);
            if (!current) continue;
            if (!property_applies(value, property.name)) continue;
            const auto key = std::string(schema->name.substr(schema->name.rfind('.') + 1)) + "." + std::string(property.name);
            theme::property(std::string(property.label).c_str());
            ImGui::PushID(static_cast<int>(property.id));
            const auto remember = [&] {
                m_layout.inspector_fields.push_back({key, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
            };
            // Angles, and rates per second or per point, are shown in degrees.
            const auto radians = property.units.starts_with("rad");
            switch (property.type) {
            case PropertyType::text: {
                auto buffer = std::get<std::string>(*current);
                char text[256];
                std::snprintf(text, sizeof(text), "%s", buffer.c_str());
                if (ImGui::InputText("##text", text, sizeof(text))) edit_property(id, value, property.id, std::string(text));
                remember();
                track_edit(group);
                break;
            }
            case PropertyType::boolean: {
                auto checked = std::get<bool>(*current);
                if (ImGui::Checkbox("##toggle", &checked)) edit_property(id, value, property.id, checked);
                remember();
                break;
            }
            case PropertyType::scalar: {
                auto number = std::get<float>(*current) * (radians ? degrees_per_radian : 1.0f);
                auto minimum = 0.0f, maximum = 0.0f;
                limits(property.range, radians ? degrees_per_radian : 1.0f, minimum, maximum);
                const auto format = radians ? (std::abs(std::get<float>(property.default_value)) < 0.1f ? "%.3f\xC2\xB0" : "%.1f\xC2\xB0") +
                                                  std::string(property.units.substr(3))
                    : property.units.empty() || property.units.size() > 3 ? std::string("%.3f")
                    : "%.3f " + std::string(property.units);
                if (ImGui::DragFloat("##number", &number, radians ? 0.25f : 0.01f, minimum, maximum, format.c_str(),
                                     ImGuiSliderFlags_AlwaysClamp) && !typing_into_last_item())
                    edit_property(id, value, property.id, number / (radians ? degrees_per_radian : 1.0f));
                remember();
                track_edit(group);
                break;
            }
            case PropertyType::vector3: {
                auto vector = std::get<math::Vec3>(*current);
                if (property.presentation == PropertyPresentation::color) {
                    float rgb[3] = {vector.x, vector.y, vector.z};
                    if (ImGui::ColorEdit3("##color", rgb, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
                                                          ImGuiColorEditFlags_NoLabel))
                        edit_property(id, value, property.id, math::Vec3{rgb[0], rgb[1], rgb[2]});
                    remember();
                    track_edit(group);
                    break;
                }
                float values[3] = {vector.x, vector.y, vector.z};
                auto minimum = 0.0f, maximum = 0.0f;
                limits(property.range, 1.0f, minimum, maximum);
                auto activated = false, deactivated = false;
                const auto changed = axis_fields("vector", values, 0.01f, minimum, maximum, "%.3f", m_layout, key,
                                                 activated, deactivated);
                if (activated && !m_edit_group_open) { m_scene->begin_group(group); m_edit_group_open = true; }
                if (changed) edit_property(id, value, property.id, math::Vec3{values[0], values[1], values[2]});
                if (deactivated && m_edit_group_open) { m_scene->end_group(); m_edit_group_open = false; }
                break;
            }
            case PropertyType::quaternion: {
                // Euler degrees for editing; the cache keeps angles stable while a drag crosses +-180.
                const auto rotation = std::get<math::Quat>(*current);
                if (m_euler_entity != id || !m_edit_group_open) {
                    m_euler = euler_degrees(rotation);
                    m_euler_entity = id;
                }
                float values[3] = {m_euler.x, m_euler.y, m_euler.z};
                auto activated = false, deactivated = false;
                const auto changed = axis_fields("rotation", values, 0.25f, -FLT_MAX, FLT_MAX, "%.1f\xC2\xB0", m_layout, key,
                                                 activated, deactivated);
                if (activated && !m_edit_group_open) { m_scene->begin_group(group); m_edit_group_open = true; }
                if (changed) {
                    m_euler = {values[0], values[1], values[2]};
                    edit_property(id, value, property.id, from_euler_degrees(m_euler));
                }
                if (deactivated && m_edit_group_open) { m_scene->end_group(); m_edit_group_open = false; }
                break;
            }
            case PropertyType::choice: {
                const auto choice = std::get<ChoiceValue>(*current).value;
                const auto selected = std::ranges::find(property.choices, choice, &EnumOption::value);
                const auto preview = selected != property.choices.end() ? std::string(selected->label) : "?";
                if (ImGui::BeginCombo("##choice", preview.c_str())) {
                    for (const auto& option : property.choices)
                        if (ImGui::Selectable(std::string(option.label).c_str(), option.value == choice))
                            edit_property(id, value, property.id, ChoiceValue{option.value});
                    ImGui::EndCombo();
                }
                remember();
                break;
            }
            case PropertyType::integer: {
                auto number = std::get<int32_t>(*current);
                if (property.presentation == PropertyPresentation::collision_group) {
                    const auto groups = collision_groups();
                    if (ImGui::BeginCombo("##group", collision_group_label(groups, size_t(number)).c_str())) {
                        for (int32_t group = 0; group < int32_t(collision_group_names); ++group) {
                            ImGui::PushID(group);
                            if (ImGui::Selectable(collision_group_label(groups, size_t(group)).c_str(), group == number))
                                edit_property(id, value, property.id, group);
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    remember();
                    break;
                }
                const auto minimum = property.range.minimum ? int(*property.range.minimum) : INT32_MIN;
                const auto maximum = property.range.maximum ? int(*property.range.maximum) : INT32_MAX;
                if (ImGui::DragInt("##integer", &number, 0.1f, minimum, maximum, "%d", ImGuiSliderFlags_AlwaysClamp) &&
                    !typing_into_last_item())
                    edit_property(id, value, property.id, number);
                remember();
                track_edit(group);
                break;
            }
            case PropertyType::flags: {
                // Collision masks: one check box per group, named as in the project.
                const auto mask = std::get<uint32_t>(*current);
                const auto groups = collision_groups();
                if (ImGui::BeginCombo("##mask", mask_summary(mask, groups).c_str())) {
                    for (size_t group = 0; group < collision_group_names; ++group) {
                        ImGui::PushID(int(group));
                        auto included = (mask >> group & 1u) != 0;
                        if (ImGui::Checkbox(collision_group_label(groups, group).c_str(), &included))
                            edit_property(id, value, property.id, included ? mask | (1u << group) : mask & ~(1u << group));
                        ImGui::PopID();
                    }
                    ImGui::Separator();
                    if (ImGui::Selectable("All groups", false, ImGuiSelectableFlags_NoAutoClosePopups))
                        edit_property(id, value, property.id, uint32_t{0xFFFF});
                    if (ImGui::Selectable("None", false, ImGuiSelectableFlags_NoAutoClosePopups))
                        edit_property(id, value, property.id, uint32_t{0});
                    ImGui::EndCombo();
                }
                remember();
                break;
            }
            case PropertyType::mesh_ref:
            case PropertyType::material_ref: {
                const auto mesh = property.type == PropertyType::mesh_ref;
                const auto asset = mesh ? std::get<AssetRef<MeshAsset>>(*current).id : std::get<AssetRef<MaterialAsset>>(*current).id;
                auto preview = std::string("None");
                auto missing = false;
                auto problem = std::string{}; // why a cataloged asset cannot be used
                if (asset.valid()) {
                    const auto info = m_assets ? m_assets->info(asset) : std::nullopt;
                    missing = !info;
                    preview = info ? info->record.path.stem().string() : "Missing " + id_text(asset.high, asset.low);
                    if (info && info->diagnostic) problem = info->diagnostic.message;
                }
                if (missing || !problem.empty())
                    ImGui::PushStyleColor(ImGuiCol_Text, missing ? theme::color::warning : theme::color::danger);
                const auto open = ImGui::BeginCombo("##asset", preview.c_str());
                if (missing || !problem.empty()) ImGui::PopStyleColor();
                if (!problem.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", problem.c_str());
                else if (missing && ImGui::IsItemHovered())
                    ImGui::SetTooltip("This asset is not in the project's catalog; saving is refused until it is replaced");
                // Dropping an asset of the right kind from the Assets panel chooses it.
                if (ImGui::BeginDragDropTarget()) {
                    if (const auto* dragged = ImGui::GetDragDropPayload(); dragged && dragged->IsDataType("MAYA_ASSET")) {
                        auto payload = AssetPayload{};
                        std::memcpy(&payload, dragged->Data, sizeof(payload));
                        if ((payload.kind == AssetKind::mesh) == mesh && ImGui::AcceptDragDropPayload("MAYA_ASSET")) {
                            if (mesh) edit_property(id, value, property.id, AssetRef<MeshAsset>{payload.id});
                            else edit_property(id, value, property.id, AssetRef<MaterialAsset>{payload.id});
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (open) {
                    const auto choose = [&](AssetId chosen) {
                        if (mesh) edit_property(id, value, property.id, AssetRef<MeshAsset>{chosen});
                        else edit_property(id, value, property.id, AssetRef<MaterialAsset>{chosen});
                    };
                    if (ImGui::Selectable("None", !asset.valid())) choose({});
                    if (m_assets)
                        for (const auto& record : m_assets->records()) {
                            if ((record.kind == AssetKind::mesh) != mesh) continue;
                            ImGui::PushID(static_cast<int>(record.id.low));
                            if (ImGui::Selectable(record.path.stem().string().c_str(), record.id == asset)) choose(record.id);
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", record.path.generic_string().c_str());
                            ImGui::PopID();
                        }
                    ImGui::EndCombo();
                }
                remember();
                break;
            }
            }
            if (!property.description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s", std::string(property.description).c_str());
            ImGui::PopID();
        }
        theme::end_properties();
    }
    if (const auto note = physics_note(id, component); !note.text.empty()) {
        icon_text(note.warning ? icon::warning : icon::info, note.warning ? theme::color::warning : theme::color::faint, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
        ImGui::TextWrapped("%s", note.text.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

CollisionGroupNames EditorShell::collision_groups() const {
    return m_project ? m_project->settings.collision_groups : default_collision_groups();
}

EditorShell::PhysicsNote EditorShell::physics_note(EntityId id, ComponentId component) const {
    if (!m_scene) return {};
    const auto has = [&](EntityId entity, ComponentId wanted) {
        const auto* record = m_scene->record(entity);
        return record && std::ranges::any_of(record->components, [&](const ComponentValue& v) { return component_id(v) == wanted; });
    };
    if (component == ComponentId::rigid_body) {
        // Its shape is its collider and the colliders below it that have no rigid body of their own.
        const auto collider_below = [&](auto&& self, EntityId parent) -> bool {
            for (const auto child : m_scene->children(parent)) {
                if (has(child, ComponentId::rigid_body)) continue;
                if (has(child, ComponentId::collider) || self(self, child)) return true;
            }
            return false;
        };
        if (!has(id, ComponentId::collider) && !collider_below(collider_below, id))
            return {"Add a collider to this entity or an entity below it; Play needs one to make the body.", true};
        if (m_scene->record(id)->parent)
            return {"A rigid body must be on a root entity; Play refuses it here.", true};
        const auto* record = m_scene->record(id);
        for (const auto& value : record->components)
            if (const auto* transform = std::get_if<TransformComponent>(&value); transform && !unit_scale(transform->scale))
                return {"A rigid body needs unit scale; Play refuses it. Size its colliders instead, or put the scaled "
                        "mesh on an entity below it.", true};
    } else if (component == ComponentId::collider && !has(id, ComponentId::rigid_body)) {
        for (auto parent = m_scene->record(id)->parent; parent; parent = m_scene->record(*parent)->parent)
            if (has(*parent, ComponentId::rigid_body))
                return {"Part of the rigid body on " + m_scene->display_name(*parent) + ".", false};
    }
    return {};
}

void EditorShell::draw_inspector() {
    const auto open = begin_panel(inspector_title);
    m_layout.inspector_fields.clear();
    if (open) {
        const auto primary = m_scene ? m_scene->primary() : std::nullopt;
        if (!primary) {
            theme::caption(m_fonts, "SELECTION");
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::muted);
            ImGui::TextWrapped("Nothing selected.");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::color::faint);
            ImGui::TextWrapped("Click an object in the viewport or a row in the hierarchy.");
            ImGui::PopStyleColor();
        } else {
            const auto id = *primary;
            const auto count = m_scene->selection().size();
            theme::caption(m_fonts, "SELECTION", count > 1 ? (std::to_string(count) + " selected, editing the last").c_str() : nullptr);
            // While playing, the play World's live values, read-only; the authored ones return at Stop.
            auto components = m_scene->record(id)->components; // a copy, since an edit replaces the record
            if (m_play) {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::color::accent);
                ImGui::TextWrapped("%s", "Live values while playing. Stop to edit.");
                ImGui::PopStyleColor();
                if (const auto entity = m_play->world().find(id)) {
                    components.clear();
                    for (const auto& schema : component_schemas())
                        if (auto value = read_component(m_play->world(), *entity, schema.id)) components.push_back(std::move(*value));
                }
            }
            ImGui::BeginDisabled(m_play != nullptr);
            // Name: a large field, committed as one step when editing ends.
            if (m_name_entity != id || !ImGui::IsAnyItemActive())
                std::snprintf(m_name_buffer, sizeof(m_name_buffer), "%s", m_scene->display_name(id).c_str());
            m_name_entity = id;
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushFont(m_fonts.strong);
            ImGui::InputText("##name", m_name_buffer, sizeof(m_name_buffer));
            ImGui::PopFont();
            m_layout.inspector_fields.push_back({"name", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const auto renamed = m_scene->rename(id, m_name_buffer);
                if (!renamed && renamed.error != "Nothing to change") { m_edit_error = renamed.error; m_edit_error_entity = id; }
            }
            theme::mono_text(m_fonts, id_text(id.high, id.low).c_str(), true);
            if (!m_edit_error.empty() && m_edit_error_entity == id) {
                ImGui::Dummy({0.0f, 2.0f});
                icon_text(icon::warning, theme::color::danger, 6.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::color::danger);
                ImGui::TextWrapped("%s", m_edit_error.c_str());
                ImGui::PopStyleColor();
            }
            // Components, in schema order.
            for (const auto& value : components) {
                if (component_id(value) == ComponentId::name) continue;
                ImGui::Separator();
                draw_component(id, value);
            }
            ImGui::Separator();
            ImGui::Dummy({0.0f, 4.0f});
            if (ImGui::Button((std::string(icon::plus) + "  Add component").c_str(), {-FLT_MIN, 0.0f})) ImGui::OpenPopup("add");
            if (ImGui::BeginPopup("add")) {
                for (const auto& schema : component_schemas()) {
                    if (schema.id == ComponentId::name) continue;
                    const auto present = std::ranges::any_of(components, [&](const ComponentValue& v) { return component_id(v) == schema.id; });
                    if (ImGui::MenuItem((std::string(component_icon(schema.id)) + "  " + std::string(schema.label)).c_str(),
                                        nullptr, false, !present)) {
                        const auto added = m_scene->set_component(id, *default_component(schema.id));
                        if (!added) { m_edit_error = added.error; m_edit_error_entity = id; }
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
        }
        ImGui::Dummy({0.0f, 12.0f});
        theme::caption(m_fonts, "EDITOR CAMERA");
        if (theme::begin_properties("camera")) {
            theme::property("Position");
            ImGui::AlignTextToFramePadding();
            const auto& p = m_camera.position;
            theme::mono_text(m_fonts, format("%.2f %.2f %.2f", p.x, p.y, p.z).c_str());
            theme::property("Speed");
            ImGui::InputFloat("##speed", &m_camera.speed, 0.0f, 0.0f, "%.2f m/s");
            m_layout.camera_speed_min = ImGui::GetItemRectMin();
            m_layout.camera_speed_max = ImGui::GetItemRectMax();
            if (!std::isfinite(m_camera.speed)) m_camera.speed = 3.0f;
            m_camera.speed = std::clamp(m_camera.speed, 0.1f, 100.0f);
            theme::property("Field of view");
            auto fov = m_camera.camera.vertical_fov * degrees_per_radian;
            if (ImGui::InputFloat("##fov", &fov, 0.0f, 0.0f, "%.0f\xC2\xB0") && std::isfinite(fov))
                m_camera.camera.vertical_fov = std::clamp(fov, 20.0f, 120.0f) / degrees_per_radian;
            theme::end_properties();
        }
    }
    ImGui::End();
}

} // namespace maya::editor
