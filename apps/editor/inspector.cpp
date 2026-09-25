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
    }
    return icon::circle_dashed;
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
            // Light properties that the light's kind does not use stay hidden (see their descriptions).
            if (component == ComponentId::light) {
                const auto kind = std::get<LightComponent>(value).kind;
                if (property.name == "range" && kind == LightKind::directional) continue;
                if ((property.name == "inner_cone" || property.name == "outer_cone") && kind != LightKind::spot) continue;
            }
            const auto key = std::string(schema->name.substr(schema->name.rfind('.') + 1)) + "." + std::string(property.name);
            theme::property(std::string(property.label).c_str());
            ImGui::PushID(static_cast<int>(property.id));
            const auto remember = [&] {
                m_layout.inspector_fields.push_back({key, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
            };
            const auto radians = property.units == "rad";
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
                const auto format = radians ? std::string("%.1f\xC2\xB0")
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
            case PropertyType::light_kind: {
                const auto kind = std::get<LightKind>(*current);
                const auto selected = std::ranges::find(property.choices, kind, &EnumOption::value);
                const auto preview = selected != property.choices.end() ? std::string(selected->label) : "?";
                if (ImGui::BeginCombo("##choice", preview.c_str())) {
                    for (const auto& option : property.choices)
                        if (ImGui::Selectable(std::string(option.label).c_str(), option.value == kind))
                            edit_property(id, value, property.id, option.value);
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
    ImGui::PopID();
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
            // Components, in schema order; copies, since an edit replaces the record.
            const auto components = m_scene->record(id)->components;
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
