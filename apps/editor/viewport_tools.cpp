// Viewport tools: transform gizmo, picking, camera/light icons, selection outline, and the tool bar.
#include "editor_shell.hpp"
#include "shell_detail.hpp"
#include <ImGuizmo.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace maya::editor {
using namespace detail;
namespace {
constexpr float icon_half = 12.0f; // points: half the side of a camera or light icon, which is also where it is picked

ImVec4 v(ImU32 color, float alpha = 1.0f) {
    auto value = ImGui::ColorConvertU32ToFloat4(color);
    value.w *= alpha;
    return value;
}

math::Vec3 transform_point(const math::Mat4& m, const math::Vec3& p) {
    const auto r = m * math::Vec4(p, 1.0f);
    return {r.x, r.y, r.z};
}
} // namespace

void detail::style_gizmo() {
    auto& style = ImGuizmo::GetStyle();
    style.TranslationLineThickness = 2.5f;
    style.TranslationLineArrowSize = 7.0f;
    style.RotationLineThickness = 2.5f;
    style.RotationOuterLineThickness = 2.0f;
    style.ScaleLineThickness = 2.5f;
    style.ScaleLineCircleSize = 6.0f;
    style.CenterCircleSize = 5.0f;
    auto* c = style.Colors;
    c[ImGuizmo::DIRECTION_X] = v(theme::color::rgb(0xF2616B));
    c[ImGuizmo::DIRECTION_Y] = v(theme::color::rgb(0x4ADE80));
    c[ImGuizmo::DIRECTION_Z] = v(theme::color::accent);
    c[ImGuizmo::PLANE_X] = v(theme::color::rgb(0xF2616B), 0.35f);
    c[ImGuizmo::PLANE_Y] = v(theme::color::rgb(0x4ADE80), 0.35f);
    c[ImGuizmo::PLANE_Z] = v(theme::color::accent, 0.35f);
    c[ImGuizmo::SELECTION] = v(theme::color::rgb(0xFFD166));
    c[ImGuizmo::INACTIVE] = v(theme::color::rgb(0x9A9CA3), 0.6f);
    c[ImGuizmo::TRANSLATION_LINE] = v(theme::color::rgb(0xFFFFFF), 0.55f);
    c[ImGuizmo::SCALE_LINE] = v(theme::color::rgb(0xFFFFFF), 0.55f);
    c[ImGuizmo::ROTATION_USING_BORDER] = v(theme::color::rgb(0xFFD166));
    c[ImGuizmo::ROTATION_USING_FILL] = v(theme::color::rgb(0xFFD166), 0.25f);
    c[ImGuizmo::HATCHED_AXIS_LINES] = v(theme::color::rgb(0xFFFFFF), 0.25f);
    c[ImGuizmo::TEXT] = v(theme::color::text);
    c[ImGuizmo::TEXT_SHADOW] = v(theme::color::background, 0.8f);
}

bool EditorShell::apply_world_matrix(EntityId id, const math::Mat4& world) {
    auto& scene = *m_scene;
    const auto* entity = scene.record(id);
    if (!entity) return false;
    // Respect the hierarchy: the new local transform is inverse(parent world) x the new world matrix.
    auto local = std::optional{world};
    if (entity->parent) {
        const auto parent = scene.world().find(*entity->parent);
        const auto parent_world = parent ? scene.world().world_matrix(*parent) : std::nullopt;
        const auto inverse = parent_world ? inverse_affine(*parent_world) : std::nullopt;
        local = inverse ? compose_affine(*inverse, world) : std::nullopt;
    }
    const auto transform = local ? decompose_transform(*local) : std::nullopt;
    if (!transform) {
        m_edit_error = "The parent's transform cannot represent this pose (it would need shear or a negative scale)";
        m_edit_error_entity = id;
        return false;
    }
    const auto result = scene.set_component(id, *transform);
    if (!result && result.error != "Nothing to change") {
        m_edit_error = result.error;
        m_edit_error_entity = id;
        return false;
    }
    m_edit_error.clear();
    return true;
}

math::Vec3 EditorShell::navigation_pivot(ImVec2 point) const {
    // While playing, the Scene view shows the play World; the selection's IDs name entities in both.
    const auto* world = m_play ? &m_play->world() : m_scene ? &m_scene->world() : nullptr;
    if (world && m_scene && !m_scene->selection().empty()) {
        auto sum = math::Vec3(0.0f);
        auto count = 0;
        for (const auto id : m_scene->selection()) {
            const auto handle = world->find(id);
            const auto matrix = handle ? world->world_matrix(*handle) : std::nullopt;
            if (!matrix) continue;
            auto center = transform_point(*matrix, {0.0f, 0.0f, 0.0f});
            if (m_snapshot && !m_play)
                for (const auto& instance : m_snapshot->instances)
                    if (instance.entity == id)
                        if (const auto& geometry = m_snapshot->meshes[instance.mesh].value().geometry(); !geometry.empty())
                            center = (transform_point(*matrix, geometry.min) + transform_point(*matrix, geometry.max)) * 0.5f;
            sum += center;
            ++count;
        }
        if (count > 0) return sum * (1.0f / float(count));
    }
    if (const auto ray = viewport_ray(point)) {
        if (m_snapshot && !m_play)
            if (const auto hits = pick_meshes(*m_snapshot, *ray); !hits.empty()) return ray->origin + ray->direction * hits.front().distance;
        if (ray->direction.y < -1e-3f) // the ground plane, y = 0, within reach
            if (const auto t = -ray->origin.y / ray->direction.y; t > 0.0f && t < 500.0f) return ray->origin + ray->direction * t;
    }
    return m_camera.position + m_camera.forward() * m_pivot_distance;
}

void EditorShell::start_navigation(NavigationMode mode, ImVec2 point) {
    if (mode == NavigationMode::fly || mode == NavigationMode::none) return;
    m_camera.pivot = navigation_pivot(point);
    // A pan moves what is at the pivot's depth exactly with the pointer.
    const auto depth = std::max(math::Vec3::dot(m_camera.pivot - m_camera.position, m_camera.forward()), 0.05f);
    const auto height = m_layout.viewport_max.y - m_layout.viewport_min.y;
    m_camera.pan_scale = height > 0.0f ? 2.0f * depth * std::tan(m_camera.camera.vertical_fov * 0.5f) / height : 0.01f;
}

void EditorShell::frame_selection() {
    const auto primary = m_scene ? m_scene->primary() : std::nullopt;
    if (!primary) return;
    const auto handle = m_scene->world().find(*primary);
    const auto world = handle ? m_scene->world().world_matrix(*handle) : std::nullopt;
    if (!world) return;
    // Look at the entity from the current direction, at a distance that fits its bounds.
    auto center = transform_point(*world, {0.0f, 0.0f, 0.0f});
    auto radius = 1.0f;
    if (m_snapshot)
        for (const auto& instance : m_snapshot->instances)
            if (instance.entity == *primary) {
                const auto& geometry = m_snapshot->meshes[instance.mesh].value().geometry();
                if (geometry.empty()) break;
                const auto low = transform_point(*world, geometry.min), high = transform_point(*world, geometry.max);
                center = (low + high) * 0.5f;
                radius = std::max((high - low).length() * 0.5f, 0.1f);
            }
    const auto distance = radius / std::sin(m_camera.camera.vertical_fov * 0.5f) * 1.1f;
    m_camera.position = center - m_camera.forward() * distance;
    m_pivot_distance = distance;
}

void EditorShell::pick_at(ImVec2 point, const RenderView& view, ImVec2 min, ImVec2 max) {
    auto& scene = *m_scene;
    const auto additive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
    // Camera and light icons sit on top of the scene, so they win anywhere on their square.
    auto hits = std::vector<EntityId>{};
    for (const auto& [id, at] : m_layout.icons)
        if (std::abs(at.x - point.x) <= icon_half && std::abs(at.y - point.y) <= icon_half) hits.push_back(id);
    if (hits.empty() && m_snapshot) {
        const auto x = (point.x - min.x) / (max.x - min.x) * 2.0f - 1.0f;
        const auto y = 1.0f - (point.y - min.y) / (max.y - min.y) * 2.0f;
        const auto ray = view_ray(view, m_camera.pose(), m_camera.camera.vertical_fov, x, y);
        for (const auto& hit : pick_meshes(*m_snapshot, ray)) hits.push_back(hit.entity);
    }
    if (hits.empty()) {
        if (!additive) scene.clear_selection();
        m_pick_hits.clear();
        return;
    }
    // Clicking the same spot again steps through overlapping objects, nearest first.
    const auto dx = point.x - m_pick_point.x, dy = point.y - m_pick_point.y;
    const auto same_spot = dx * dx + dy * dy <= 16.0f && hits == m_pick_hits;
    m_pick_index = same_spot ? (m_pick_index + 1) % hits.size() : 0;
    m_pick_hits = hits;
    m_pick_point = point;
    scene.select(hits[m_pick_index], additive ? SelectMode::toggle : SelectMode::replace);
    m_reveal = hits[m_pick_index]; // show it in the hierarchy
}

void EditorShell::draw_viewport_tools(const RenderView& view, ImVec2 min, ImVec2 max) {
    auto& scene = *m_scene;
    auto* draw = ImGui::GetWindowDrawList();
    const auto& io = ImGui::GetIO();
    const auto view_projection = view.matrices.view_projection;
    const auto project = [&](const math::Vec3& p) -> std::optional<ImVec2> {
        const auto clip = view_projection * math::Vec4(p, 1.0f);
        if (clip.w <= 1e-4f) return std::nullopt; // behind the camera
        return ImVec2{min.x + (clip.x / clip.w * 0.5f + 0.5f) * (max.x - min.x),
                      min.y + (0.5f - clip.y / clip.w * 0.5f) * (max.y - min.y)};
    };
    draw->PushClipRect(min, max, true);

    // Selection outline: each selected mesh's bounding box, in its own orientation.
    if (m_snapshot)
        for (const auto& instance : m_snapshot->instances) {
            if (!scene.selected(instance.entity)) continue;
            const auto handle = scene.world().find(instance.entity);
            const auto world = handle ? scene.world().world_matrix(*handle) : std::nullopt;
            const auto& geometry = m_snapshot->meshes[instance.mesh].value().geometry();
            if (!world || geometry.empty()) continue;
            const auto& a = geometry.min;
            const auto& b = geometry.max;
            std::optional<ImVec2> corners[8];
            for (int i = 0; i < 8; ++i)
                corners[i] = project(transform_point(*world, {i & 1 ? b.x : a.x, i & 2 ? b.y : a.y, i & 4 ? b.z : a.z}));
            constexpr int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            const auto tone = scene.primary() == instance.entity ? theme::color::rgb(0xFFD166) : theme::color::rgb(0xFFD166, 150);
            for (const auto& [from, to] : edges)
                if (corners[from] && corners[to]) draw->AddLine(*corners[from], *corners[to], tone, 1.5f);
        }

    // Camera and light icons at their entities' positions, styled like the tool bar's buttons; a
    // selected one takes the selection outline's colour.
    m_layout.icons.clear();
    scene.world().for_each_entity([&](EntityHandle entity) {
        const auto camera = scene.world().has<CameraComponent>(entity), light = scene.world().has<LightComponent>(entity);
        if (!camera && !light) return;
        const auto world = scene.world().world_matrix(entity);
        const auto at = world ? project(transform_point(*world, {0.0f, 0.0f, 0.0f})) : std::nullopt;
        if (!at || at->x < min.x || at->x >= max.x || at->y < min.y || at->y >= max.y) return; // off-screen
        const auto id = *scene.world().persistent_id(entity);
        const auto selected = scene.selected(id);
        const auto corner = ImVec2{std::round(at->x) - icon_half, std::round(at->y) - icon_half};
        const auto far = ImVec2{corner.x + icon_half * 2.0f, corner.y + icon_half * 2.0f};
        draw->AddRectFilled(corner, far, theme::color::rgb(0x0B0C0E, 200), 6.0f);
        draw->AddRect(corner, far, selected ? theme::color::rgb(0xFFD166) : theme::color::rgb(0xFFFFFF, 28), 6.0f, 0,
                      selected ? 1.5f : 1.0f);
        const auto* glyph = camera ? icon::video_camera : icon::sun;
        draw->AddText(glyph_origin(glyph, {corner.x + icon_half, corner.y + icon_half}),
                      selected ? theme::color::rgb(0xFFD166) : theme::color::text, glyph);
        m_layout.icons.emplace_back(id, *at);
    });
    draw->PopClipRect();

    // Tool bar: move, rotate, scale, and world or local space.
    ImGui::SetCursorScreenPos({min.x + 10.0f, min.y + 10.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {7.0f, 5.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2.0f, 2.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    const auto tool = [&](const char* glyph, bool active, const char* tip) {
        ImGui::PushStyleColor(ImGuiCol_Button, active ? theme::color::rgb(0x7B8CFF, 60) : theme::color::rgb(0x0B0C0E, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::rgb(0x26282E, 230));
        ImGui::PushStyleColor(ImGuiCol_Text, active ? theme::color::text : theme::color::muted);
        const auto pressed = ImGui::Button(glyph);
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
        return pressed;
    };
    if (tool(icon::arrows_move, m_gizmo == GizmoOperation::translate, "Move   W")) m_gizmo = GizmoOperation::translate;
    if (tool(icon::rotate, m_gizmo == GizmoOperation::rotate, "Rotate   E")) m_gizmo = GizmoOperation::rotate;
    if (tool(icon::scale, m_gizmo == GizmoOperation::scale, "Scale   R")) m_gizmo = GizmoOperation::scale;
    ImGui::SameLine(0.0f, 8.0f);
    if (tool(m_gizmo_local ? icon::cube : icon::globe, false,
             m_gizmo_local ? "Local space (scale is always local)   X" : "World space   X"))
        m_gizmo_local = !m_gizmo_local;
    ImGui::SameLine(0.0f, 8.0f);
    if (tool(icon::frame, false, "Frame selection   F")) frame_selection();
    ImGui::SameLine(0.0f, 8.0f);
    if (tool(icon::cube_transparent, m_collider_editing, "Edit collider   C")) set_collider_editing(!m_collider_editing);
    m_layout.controls.push_back({"tool.collider", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    if (tool(icon::eye, m_preferences.physics_debug.any() || m_preferences.exposure_view != ExposureView::none ||
                            m_preferences.shadow_view != ShadowView::none, "Debug views"))
        ImGui::OpenPopup("physics-debug");
    m_layout.controls.push_back({"tool.physics-debug", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    ImGui::PopStyleVar(3);
    ImGui::NewLine();
    const auto toolbar_hovered = ImGui::IsAnyItemHovered();
    draw_physics_debug_menu();

    // Gizmo on the primary selection.
    m_layout.gizmo_origin.reset();
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(min.x, min.y, max.x - min.x, max.y - min.y);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetGizmoSizeClipSpace(0.16f);
    const auto primary = scene.primary();
    const auto* record = primary ? scene.record(*primary) : nullptr;
    const auto has_transform = record && std::ranges::any_of(record->components, [](const ComponentValue& value) {
        return component_id(value) == ComponentId::transform;
    });
    auto shown = false;
    const auto handles = m_collider_editing && draw_collider_handles(view, min, max);
    if (has_transform && !handles) {
        const auto handle = scene.world().find(*primary);
        const auto world = handle ? scene.world().world_matrix(*handle) : std::nullopt;
        if (world) {
            shown = true;
            float matrix[16];
            std::memcpy(matrix, world->elements, sizeof(matrix));
            const auto operation = m_gizmo == GizmoOperation::translate ? ImGuizmo::TRANSLATE
                : m_gizmo == GizmoOperation::rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
            const auto mode = m_gizmo_local || m_gizmo == GizmoOperation::scale ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
            // Hold Command (Ctrl) to snap: 0.25 m, 15 degrees, or 0.1 scale.
            const float snap_values[3] = {m_gizmo == GizmoOperation::rotate ? 15.0f : m_gizmo == GizmoOperation::scale ? 0.1f : 0.25f,
                                          m_gizmo == GizmoOperation::rotate ? 15.0f : m_gizmo == GizmoOperation::scale ? 0.1f : 0.25f,
                                          m_gizmo == GizmoOperation::rotate ? 15.0f : m_gizmo == GizmoOperation::scale ? 0.1f : 0.25f};
            const auto changed = ImGuizmo::Manipulate(view.matrices.view.elements, view.matrices.projection.elements,
                                                      operation, mode, matrix, nullptr, io.KeyCtrl ? snap_values : nullptr);
            const auto using_now = ImGuizmo::IsUsing();
            if (using_now && !m_gizmo_using) {
                static constexpr const char* verbs[] = {"Move ", "Rotate ", "Scale "};
                scene.begin_group(verbs[static_cast<int>(m_gizmo)] + scene.display_name(*primary));
            }
            if (changed) {
                auto moved = math::Mat4{};
                std::memcpy(moved.elements, matrix, sizeof(matrix));
                apply_world_matrix(*primary, moved);
            }
            if (!using_now && m_gizmo_using) scene.end_group();
            m_gizmo_using = using_now;
            m_gizmo_hovered = ImGuizmo::IsOver();
            m_layout.gizmo_origin = project(transform_point(*world, {0.0f, 0.0f, 0.0f}));
        }
    }
    if (!shown) {
        if (m_gizmo_using) scene.end_group(); // the selection changed or vanished mid-drag
        m_gizmo_using = m_gizmo_hovered = false;
    }

    // Click to select (the gizmo and tool bar take their own clicks).
    const auto over_image = io.MousePos.x >= min.x && io.MousePos.x < max.x && io.MousePos.y >= min.y && io.MousePos.y < max.y;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && over_image && ImGui::IsWindowHovered() && !toolbar_hovered &&
        !m_gizmo_hovered && !m_gizmo_using && !m_handle_hovered && !m_collider_drag)
        pick_at(io.MousePos, view, min, max);

    // Tool keys while the pointer is over the viewport and no text field has the keyboard.
    // An active control (such as a text field being typed into) keeps the keys.
    if (ImGui::IsWindowHovered() && !m_router.navigating() && !m_gizmo_using && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmo = GizmoOperation::translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmo = GizmoOperation::rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmo = GizmoOperation::scale;
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) m_gizmo_local = !m_gizmo_local;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_selection();
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) set_collider_editing(!m_collider_editing);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            if (const auto result = scene.delete_selection(); !result && result.error != "Nothing is selected")
                report(result, "Delete");
        }
    }
}


// The physics debug views menu: a check for each category and for each collision group, saved as
// preferences as they change.
void EditorShell::draw_physics_debug_menu() {
    if (!ImGui::BeginPopup("physics-debug")) return;
    auto options = m_preferences.physics_debug;
    const auto remember = [&](const char* key) { m_layout.controls.push_back({key, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()}); };
    // Exposure views replace the image with its exposed luminance (docs/renderer.md#exposure-views).
    theme::caption(m_fonts, "EXPOSURE");
    const auto exposure = [&](ExposureView view, const char* label, const char* key, const char* tip) {
        if (ImGui::RadioButton(label, m_preferences.exposure_view == view)) set_exposure_view(view);
        remember(key);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    exposure(ExposureView::none, "Image", "debug.exposure.none", "The tone-mapped image.");
    ImGui::SameLine();
    exposure(ExposureView::luminance, "Luminance", "debug.exposure.luminance",
             "Exposed luminance in grey by stops from middle grey: black at -8, white at +8.");
    ImGui::SameLine();
    exposure(ExposureView::false_color, "False color", "debug.exposure.false-color",
             "A color per band of stops from middle grey: blues under, grey within half a stop, yellow to red over, "
             "pink past +6.");
    ImGui::Dummy({0.0f, 4.0f});
    // Shadow views tint the sun's cascades or checker their texels (docs/renderer.md#shadow-views).
    theme::caption(m_fonts, "SHADOWS");
    const auto shadows = [&](ShadowView view, const char* label, const char* key, const char* tip) {
        if (ImGui::RadioButton(label, m_preferences.shadow_view == view)) set_shadow_view(view);
        remember(key);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    shadows(ShadowView::none, "Lit", "debug.shadows.none", "The lit image.");
    ImGui::SameLine();
    shadows(ShadowView::cascades, "Cascades", "debug.shadows.cascades",
            "Tints each of the sun's four shadow cascades: red, green, blue, yellow, nearest first.");
    ImGui::SameLine();
    shadows(ShadowView::texels, "Texels", "debug.shadows.texels",
            "Checkers the shadow maps' texels where they land: big squares mean blocky shadows there.");
    ImGui::Dummy({0.0f, 4.0f});
    theme::caption(m_fonts, "PHYSICS DEBUG");
    const auto category = [&](PhysicsDebugCategory value, const char* label, const char* key, const char* tip) {
        auto on = options.has(value);
        if (ImGui::Checkbox(label, &on)) options.categories = on ? options.categories | uint8_t(value) : options.categories & ~uint8_t(value);
        remember(key);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    category(PhysicsDebugCategory::colliders, "Colliders", "debug.colliders", "Outlines of solid colliders.");
    category(PhysicsDebugCategory::body_state, "Body state", "debug.body-state",
             "Colors outlines by body: static, kinematic, active, or sleeping (in Play).");
    category(PhysicsDebugCategory::triggers, "Triggers", "debug.triggers", "Outlines of sensor colliders.");
    category(PhysicsDebugCategory::contacts, "Contacts", "debug.contacts", "Contact points and normals of the last step, in Play.");
    category(PhysicsDebugCategory::queries, "Queries", "debug.queries", "The last tick's raycasts, shape casts, and overlaps, and their hits, in Play.");
    ImGui::Dummy({0.0f, 4.0f});
    theme::caption(m_fonts, "COLLISION GROUPS");
    if (ImGui::SmallButton("All")) options.groups = all_collision_groups;
    remember("debug.groups.all");
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) options.groups = 0;
    remember("debug.groups.none");
    const auto names = collision_groups();
    if (ImGui::BeginTable("groups", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (uint32_t group = 0; group < collision_group_count; ++group) {
            ImGui::TableNextColumn();
            auto on = (options.groups >> group & 1u) != 0;
            ImGui::PushID(int(group));
            if (ImGui::Checkbox(collision_group_label(names, group).c_str(), &on))
                options.groups = uint16_t(on ? options.groups | 1u << group : options.groups & ~(1u << group));
            ImGui::PopID();
            m_layout.controls.push_back({"debug.group." + std::to_string(group), ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
        }
        ImGui::EndTable();
    }
    set_physics_debug(options);
    ImGui::EndPopup();
}

// Collider handles: a dot on each face (box), on each axis (sphere), or on the radius and each end
// (capsule), and a square at the centre. A face or end moves with the opposite one held still, so a
// drag changes the size and the offset together; the centre moves the offset in the plane facing
// the camera. Everything is worked out in the entity's own space, where the collider is described.
bool EditorShell::draw_collider_handles(const RenderView& view, ImVec2 min, ImVec2 max) {
    auto& scene = *m_scene;
    m_handle_hovered = false;
    const auto end_drag = [&] {
        if (m_collider_drag) scene.end_group();
        m_collider_drag.reset();
    };
    const auto primary = scene.primary();
    const auto entity = primary ? scene.world().find(*primary) : std::nullopt;
    const auto world = entity ? scene.world().world_matrix(*entity) : std::nullopt;
    const auto inverse = world ? inverse_affine(*world) : std::nullopt;
    auto collider = std::optional<ColliderComponent>{};
    if (inverse) scene.world().with<ColliderComponent>(*entity, [&](const ColliderComponent& value) { collider = value; });
    if (!collider || (m_collider_drag && m_collider_drag->entity != *primary)) {
        end_drag();
        if (!collider) return false;
    }

    struct Handle {
        std::string key;
        math::Vec3 point; // entity space
        math::Vec3 axis; // unit, entity space, pointing out; zero for the centre
    };
    const auto rotation = collider->rotation;
    const auto offset = collider->offset;
    auto handles = std::vector<Handle>{{"collider.centre", offset, {}}};
    const math::Vec3 unit_axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const char* const names[3] = {"x", "y", "z"};
    for (int axis = 0; axis < 3; ++axis) {
        const auto along = rotation.rotate(unit_axes[axis]);
        auto reach = collider->radius;
        if (collider->shape == ColliderShape::box) reach = axis == 0 ? collider->half_extents.x : axis == 1 ? collider->half_extents.y : collider->half_extents.z;
        if (collider->shape == ColliderShape::capsule && axis == 1) reach = collider->half_height + collider->radius;
        for (const auto sign : {1.0f, -1.0f})
            handles.push_back({std::string("collider.") + (sign > 0 ? "+" : "-") + names[axis], offset + along * (reach * sign), along * sign});
    }

    const auto& io = ImGui::GetIO();
    const auto project = [&](const math::Vec3& local) -> std::optional<ImVec2> {
        const auto p = transform_point(*world, local);
        const auto clip = view.matrices.view_projection * math::Vec4(p, 1.0f);
        if (clip.w <= 1e-4f) return std::nullopt;
        return ImVec2{min.x + (clip.x / clip.w * 0.5f + 0.5f) * (max.x - min.x), min.y + (0.5f - clip.y / clip.w * 0.5f) * (max.y - min.y)};
    };
    // The pointer as a ray in entity space.
    const auto local_ray = [&]() -> std::optional<Ray> {
        const auto ray = viewport_ray(io.MousePos);
        if (!ray) return std::nullopt;
        const auto o = *inverse * math::Vec4(ray->origin, 1.0f);
        const auto d = *inverse * math::Vec4(ray->direction, 0.0f);
        return Ray{{o.x, o.y, o.z}, {d.x, d.y, d.z}};
    };
    // Where the pointer is along a handle's line through `centre` (the offset when the drag began).
    const auto along_line = [&](const Ray& ray, const math::Vec3& centre, const math::Vec3& axis) -> std::optional<float> {
        const auto w = centre - ray.origin;
        const auto a = math::Vec3::dot(axis, axis), b = math::Vec3::dot(axis, ray.direction), c = math::Vec3::dot(ray.direction, ray.direction);
        const auto denominator = a * c - b * b;
        if (std::abs(denominator) < 1e-8f) return std::nullopt; // looking straight along it
        return (b * math::Vec3::dot(ray.direction, w) - c * math::Vec3::dot(axis, w)) / denominator;
    };
    // Where the pointer meets the plane through the centre that faces the camera.
    const auto forward = *inverse * math::Vec4(m_camera.forward(), 0.0f);
    const auto on_plane = [&](const Ray& ray, const math::Vec3& centre) -> std::optional<math::Vec3> {
        const auto normal = math::Vec3{forward.x, forward.y, forward.z};
        const auto facing = math::Vec3::dot(normal, ray.direction);
        if (std::abs(facing) < 1e-8f) return std::nullopt;
        const auto t = math::Vec3::dot(normal, centre - ray.origin) / facing;
        return ray.origin + ray.direction * t;
    };

    // Draw, and find the handle under the pointer.
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(min, max, true);
    const Handle* hovered = nullptr;
    auto nearest = 8.0f * 8.0f;
    const auto tone = theme::color::rgb(0xFFD166);
    for (const auto& handle : handles) {
        const auto at = project(handle.point);
        if (!at) continue;
        m_layout.controls.push_back({handle.key, {at->x - 6.0f, at->y - 6.0f}, {at->x + 6.0f, at->y + 6.0f}});
        const auto dx = io.MousePos.x - at->x, dy = io.MousePos.y - at->y;
        const auto dragging = m_collider_drag && m_collider_drag->handle == handle.key;
        if (!m_collider_drag && dx * dx + dy * dy <= nearest) {
            nearest = dx * dx + dy * dy;
            hovered = &handle;
        }
        const auto size = dragging || hovered == &handle ? 6.5f : 5.0f;
        if (handle.key == "collider.centre") {
            draw->AddRectFilled({at->x - size, at->y - size}, {at->x + size, at->y + size}, theme::color::rgb(0x0B0C0E, 220), 2.0f);
            draw->AddRect({at->x - size, at->y - size}, {at->x + size, at->y + size}, tone, 2.0f, 0, 1.5f);
        } else {
            draw->AddCircleFilled(*at, size, tone);
            draw->AddCircle(*at, size + 1.0f, theme::color::rgb(0x0B0C0E, 220), 0, 1.5f);
        }
    }
    draw->PopClipRect();
    m_handle_hovered = hovered != nullptr && ImGui::IsWindowHovered();

    // Press on a handle to start a drag: one undo step for the whole drag.
    if (!m_collider_drag && hovered && m_handle_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_router.navigating()) {
        const auto ray = local_ray();
        auto drag = ColliderDrag{*primary, hovered->key, *collider};
        auto started = false;
        if (ray && hovered->axis.length_squared() == 0.0f) {
            if (const auto point = on_plane(*ray, offset)) {
                drag.start_point = *point;
                started = true;
            }
        } else if (ray) {
            if (const auto t = along_line(*ray, offset, hovered->axis)) {
                drag.start_along = *t;
                started = true;
            }
        }
        if (started) {
            scene.begin_group((hovered->key == "collider.centre" ? "Move the collider of " : "Resize the collider of ") + scene.display_name(*primary));
            m_collider_drag = std::move(drag);
        }
    }
    if (!m_collider_drag) return true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        end_drag();
        return true;
    }

    // Dragging: the new collider, from the one at the press.
    const auto& drag = *m_collider_drag;
    const auto found = std::ranges::find(handles, drag.handle, &Handle::key);
    const auto ray = local_ray();
    if (found == handles.end() || !ray) return true;
    constexpr float smallest = 0.005f; // metres: half extents, radii, and half heights stay positive
    auto updated = drag.start;
    const auto& start = drag.start;
    if (found->axis.length_squared() == 0.0f) {
        if (const auto point = on_plane(*ray, start.offset)) updated.offset = start.offset + (*point - drag.start_point);
    } else if (const auto t = along_line(*ray, start.offset, found->axis)) {
        const auto moved = *t - drag.start_along; // how far the handle moved, outward positive
        const auto axis = drag.handle[10] == 'x' ? 0 : drag.handle[10] == 'y' ? 1 : 2; // "collider.+x"
        const auto outward = found->axis;
        if (start.shape == ColliderShape::box) {
            auto half = start.half_extents;
            auto& h = axis == 0 ? half.x : axis == 1 ? half.y : half.z;
            const auto before = h;
            h = std::max(before + moved * 0.5f, smallest);
            updated.half_extents = half;
            updated.offset = start.offset + outward * (h - before); // the opposite face stays
        } else if (start.shape == ColliderShape::capsule && axis == 1) {
            updated.half_height = std::max(start.half_height + moved * 0.5f, smallest);
            updated.offset = start.offset + outward * (updated.half_height - start.half_height); // the other end stays
        } else {
            updated.radius = std::max(start.radius + moved, smallest);
        }
    }
    const auto result = scene.set_component(*primary, updated);
    if (!result && result.error != "Nothing to change") {
        m_edit_error = result.error;
        m_edit_error_entity = *primary;
    } else {
        m_edit_error.clear();
    }
    return true;
}

} // namespace maya::editor
