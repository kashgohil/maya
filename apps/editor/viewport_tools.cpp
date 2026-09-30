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
    ImGui::PopStyleVar(3);
    ImGui::NewLine();
    const auto toolbar_hovered = ImGui::IsAnyItemHovered();

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
    if (has_transform) {
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
        !m_gizmo_hovered && !m_gizmo_using)
        pick_at(io.MousePos, view, min, max);

    // Tool keys while the pointer is over the viewport and no text field has the keyboard.
    // An active control (such as a text field being typed into) keeps the keys.
    if (ImGui::IsWindowHovered() && !m_router.navigating() && !m_gizmo_using && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmo = GizmoOperation::translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmo = GizmoOperation::rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmo = GizmoOperation::scale;
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) m_gizmo_local = !m_gizmo_local;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_selection();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            if (const auto result = scene.delete_selection(); !result && result.error != "Nothing is selected")
                report(result, "Delete");
        }
    }
}

} // namespace maya::editor
