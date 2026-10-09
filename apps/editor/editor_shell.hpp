#pragma once

#include "maya/simulation/recording.hpp"
#include "maya/simulation/scripting.hpp"
#include "editor_camera.hpp"
#include "editor_preferences.hpp"
#include "editor_theme.hpp"
#include "input_router.hpp"
#include "picking.hpp"
#include "scene_editor.hpp"
#include "texture_thumbnails.hpp"
#include "ui_renderer.hpp"
#include "maya/assets/project.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/core/system_info.hpp"
#include "maya/metrics/metrics.hpp"
#include "maya/platform/input.hpp"
#include "maya/renderer/renderer.hpp"
#include "maya/simulation/play_session.hpp"
#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

struct ImGuiContext;

namespace maya::editor {

/// TrueType data for the editor's typefaces. Any empty or unreadable entry uses ImGui's built-in font.
struct EditorFonts {
    std::string regular; // resources/fonts/Inter-Regular.ttf
    std::string semibold; // resources/fonts/Inter-SemiBold.ttf
    std::string mono; // resources/fonts/GeistMono-Regular.ttf
    std::string icons; // resources/fonts/Phosphor-Light.ttf, merged into the regular and semibold fonts
};

/// Framebuffer pixel size of a view.
struct PixelSize {
    uint32_t width = 0;
    uint32_t height = 0;
    bool empty() const noexcept { return width == 0 || height == 0; }
    auto operator<=>(const PixelSize&) const = default;
};
/// Pixel size for a panel area in points at a display scale: whole pixels, never stretched.
PixelSize viewport_pixels(float width_points, float height_points, float scale) noexcept;

/// Where the last frame placed interactive elements, in window points. Empty when not shown.
struct EditorLayout {
    struct Row { EntityId id; ImVec2 min, max; };
    struct Field { std::string key; ImVec2 min, max; }; // e.g. "transform.translation.x"
    ImVec2 viewport_min{0, 0}, viewport_max{0, 0};
    ImVec2 camera_speed_min{0, 0}, camera_speed_max{0, 0};
    std::vector<Row> hierarchy_rows; // visible hierarchy rows, top to bottom
    std::vector<Field> inspector_fields; // editable inspector controls
    std::optional<ImVec2> gizmo_origin; // the selected entity's origin on screen, when a gizmo is shown
    std::vector<std::pair<EntityId, ImVec2>> icons; // camera and light icons in the viewport
    std::vector<Field> controls; // dialog buttons and asset rows, e.g. "dialog.save", "asset.cube.obj"
    const Row* row(EntityId id) const {
        for (const auto& row : hierarchy_rows) if (row.id == id) return &row;
        return nullptr;
    }
    const Field* field(std::string_view key) const {
        for (const auto& field : inspector_fields) if (field.key == key) return &field;
        return nullptr;
    }
    const Field* control(std::string_view key) const {
        for (const auto& control : controls) if (control.key == key) return &control;
        return nullptr;
    }
};

enum class GizmoOperation { translate, rotate, scale };

enum class DiagnosticSource { scene, viewport, renderer, gpu, ui, edit, project, asset, play, script };

/// A modal dialog the editor is showing.
enum class EditorPrompt { none, unsaved_changes, save_as, notice };
struct DiagnosticEntry {
    DiagnosticSource source;
    std::string message;
    uint64_t count = 1; // repeated messages are merged
    uint64_t last_frame = 0;
};
/// Bounded log of editor problems and state changes, newest last.
class DiagnosticLog {
public:
    static constexpr size_t capacity = 200;
    void add(DiagnosticSource source, std::string message, uint64_t frame);
    const std::deque<DiagnosticEntry>& entries() const noexcept { return m_entries; }
    size_t count(DiagnosticSource source) const;

private:
    std::deque<DiagnosticEntry> m_entries;
};

/// The editor window's contents: a dockable layout with hierarchy, viewport, inspector, assets, and
/// diagnostics panels; input routing between the UI and the editor camera; and rendering of the
/// viewport and UI through the shared renderer. Owns its ImGui context. Main thread only.
class EditorShell {
public:
    EditorShell(GraphicsDevice& device, std::string renderer_shader, std::string ui_shader,
                PlatformServices services = {}, EditorFonts fonts = {});
    ~EditorShell();
    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    /// Opens a project from its file, or a directory containing project.maya: reads its catalog into a
    /// new asset registry and opens its startup scene, or a new scene when it has none. It does not ask
    /// about unsaved changes. On failure the open project and scene stay, and the reason is shown and
    /// logged. Returns whether it opened.
    bool open_project(const std::filesystem::path& path);
    const Project* project() const noexcept { return m_project ? &*m_project : nullptr; }
    /// Names collision group `index` (0 to 15) and saves the project file; an empty name leaves the
    /// group unnamed. Returns why it could not, or empty. Not an undoable scene edit.
    std::string rename_collision_group(size_t index, const std::string& name);
    /// Shows or hides the Collision groups window.
    void show_collision_groups(bool shown) { m_groups_open = shown; }
    /// Opens a scene of the open project (content-relative, or absolute inside the content root) with
    /// fresh history and selection. It does not ask about unsaved changes; request_open_scene does.
    /// On failure the current scene stays open, and the problems are shown and logged.
    bool open_scene(const std::filesystem::path& path);
    /// Replaces the open scene with a new, never-saved one holding a camera and a directional light.
    bool new_scene();
    /// Saves the open scene to `path` (content-relative, or absolute inside the content root), or to
    /// its own file when `path` is empty, creating folders as needed. Roots are saved in hierarchy
    /// order. A failed save leaves every file untouched and the scene unsaved. Returns the reason for
    /// a failure, which is also logged; empty on success.
    std::string save_scene(const std::filesystem::path& path = {});
    /// The open scene's file, or empty for a new scene that has never been saved.
    const std::filesystem::path& scene_path() const noexcept { return m_scene_path; }
    /// Scene files in the project's content root, content-relative and sorted.
    const std::vector<std::filesystem::path>& scene_files() const noexcept { return m_scene_files; }
    /// The open project's asset registry, or null when no project is open.
    AssetRegistry* assets() noexcept { return m_assets.get(); }
    /// Catalog entries whose source file is missing, as of the last open, refresh, or reload.
    const std::vector<AssetId>& missing_asset_files() const noexcept { return m_missing_files; }
    /// Rereads the catalog into a new registry (kept only if it is valid), rescans the scene files,
    /// and rechecks which asset files exist.
    void refresh_project();

    /// Opening or creating a scene, and closing, first ask about unsaved changes: Save, Don't save,
    /// or Cancel. Save on a never-saved scene asks for a path first.
    void request_open_scene(const std::filesystem::path& path);
    void request_new_scene();
    /// The host's close request. True when nothing unsaved would be lost; otherwise the editor asks,
    /// and once the changes are saved or discarded it closes through PlatformServices::request_close.
    bool request_close();
    EditorPrompt prompt() const noexcept { return m_prompt; }
    /// The message a notice or dialog is showing, e.g. why a save failed.
    const std::string& prompt_message() const noexcept { return m_prompt_message; }

    /// Plays the open scene: a new World, built from its current state (saved or not), runs the
    /// built-in systems on a fixed clock. The authored scene is locked and untouched until stop_play.
    /// Returns whether play started; a scene that cannot be built reports why. With `record`, the Play
    /// is recorded for saving and replay (docs/play.md#recording-and-replay); plain Play records nothing,
    /// since a recording costs time at Play, every 60 ticks, and at Stop.
    bool start_play(bool record = false);
    /// The last recorded Play's recording; none yet.
    const std::optional<PlayRecording>& last_recording() const noexcept { return m_last_recording; }
    /// Writes the last recording to recordings/<scene>.recording in the content folder. Returns why
    /// not, or empty.
    std::string save_recording();
    /// Plays the last recording again, from its scene with its input, and checks that it matches.
    /// A recording this build or the project's scripts and assets cannot replay is refused.
    bool replay_last_play();
    /// Drops the play World and its resources, unlocks the authored scene, and restores the selection
    /// it had at Play. The editor camera and panels stay as they are.
    void stop_play();
    void toggle_pause();
    /// While paused, runs exactly one tick on the next frame.
    void step_play();
    /// The running play session, or null while editing.
    PlaySession* play_session() noexcept { return m_play.get(); }
    /// While playing, whether the viewport shows the scene's camera (and a click gives the game the
    /// input) rather than the editor camera.
    bool game_view() const noexcept { return m_game_view; }
    /// Playing, in the game view, with a camera to show: the viewport is the game.
    bool showing_game() const noexcept { return m_play && m_game_view && m_play->camera(); }
    void set_game_view(bool game) noexcept;
    bool game_has_input() const noexcept { return m_router.game_has_input(); }

    /// Places an instance of a mesh asset at a world position, named after its file, as one undo step.
    EditResult place_mesh(AssetId mesh, const math::DVec3& position);
    /// Assigns a mesh or material asset to an entity's mesh renderer, adding one to an entity with a
    /// transform when it has none, or a script to its script component, as one undo step.
    EditResult assign_asset(EntityId entity, AssetId asset);
    /// Lights the scene with an environment: sets the scene's Environment component (the one with the
    /// lowest EntityId), or creates an entity named Environment with one, as one undo step.
    EditResult use_environment(AssetId asset);
    /// Where a mesh dropped at a viewport point lands: on the surface under it, else on the ground
    /// plane, else in front of the camera. Null when the viewport is not shown.
    /// `mesh`, when given, rests on the surface: it is lifted by how far it reaches below its origin.
    std::optional<math::DVec3> drop_point(ImVec2 point, AssetId mesh = {}) const;

    /// Imports a glTF file inside the content root (docs/editor.md#importing-models), logs what it made
    /// and any warnings, and refreshes the project; a failure shows why and changes nothing. With
    /// `place`, the imported scene is then placed in the open scene, in front of the camera.
    GltfImportResult import_model(const std::filesystem::path& path, bool place, bool notify = true);
    /// Places a copy of a scene file's entities (content-relative) in the open scene at a world
    /// position, with new IDs, as one undo step: dragging an imported model into the viewport.
    EditResult place_scene(const std::filesystem::path& relative, math::DVec3 at);
    /// glTF files in the content root, content-relative and sorted, as of the last scan.
    const std::vector<std::filesystem::path>& model_files() const noexcept { return m_model_files; }
    /// The open project's cook cache, or null when no project is open.
    const CookCache* cook_cache() const noexcept { return m_cook_cache.get(); }
    /// An asset's name for display: its file's name, or for a part of an imported file, what the
    /// import file calls it ("Panel", "Albedo").
    std::string asset_label(const AssetRecord& record) const;

    /// Routes this frame's input and builds the UI. A zero-sized (minimized) window skips the frame.
    void update(float delta_time, const std::vector<InputEvent>& events, const WindowMetrics& metrics);
    /// Inside a device frame with no pass open: renders the viewport, then the UI into `destination`
    /// (normally the window surface). Viewport problems are reported in diagnostics, not returned;
    /// the returned error means the UI pass itself failed.
    RhiDiagnostic render(TextureHandle destination);
    /// The cursor capture state to request from the host, when it changed this frame.
    std::optional<bool> take_capture_request() noexcept { return std::exchange(m_capture_request, std::nullopt); }

    bool navigating() const noexcept { return m_router.navigating(); }
    bool ui_wants_text() const noexcept { return m_ui_wants_text; }
    bool viewport_hovered() const noexcept { return m_viewport_hovered; }
    const EditorCamera& camera() const noexcept { return m_camera; }
    EditorCamera& camera() noexcept { return m_camera; }
    /// What orbit and zoom turn around, chosen when they start at a viewport point: the selection's
    /// centre; else the surface or ground under the point; else a point ahead at the last distance.
    math::DVec3 navigation_pivot(ImVec2 point) const;
    PixelSize viewport_request() const noexcept { return m_viewport_request; }
    const RenderTarget& viewport() const noexcept { return m_viewport; }
    const EditorLayout& layout() const noexcept { return m_layout; }
    const DiagnosticLog& diagnostics() const noexcept { return m_log; }
    const TextureThumbnails& thumbnails() const noexcept { return m_thumbnails; }
    const UiRenderer& ui_renderer() const noexcept { return m_ui; }
    const RenderSnapshotStats& extraction() const noexcept { return m_extraction; }
    /// The registry's loading as of this frame's update (Diagnostics' Loading row).
    const AssetLoadStats& loading() const noexcept { return m_load_stats; }
    /// The last viewport frame's lights (Diagnostics' Lights row), and its scene problems, including the
    /// lights that view left out or drew without shadows.
    struct LightCounts {
        size_t local = 0, dropped = 0, unshadowed = 0;
        bool sun = false;
    };
    const LightCounts& lights() const noexcept { return m_lights; }
    const std::vector<RenderDiagnostic>& frame_problems() const noexcept { return m_frame_problems; }
    /// The open scene's editing session, or null when no scene is open.
    SceneEditor* scene() noexcept { return m_scene.get(); }
    std::optional<EntityId> renaming() const noexcept { return m_renaming; }
    GizmoOperation gizmo_operation() const noexcept { return m_gizmo; }
    bool gizmo_local() const noexcept { return m_gizmo_local; }
    bool gizmo_hovered() const noexcept { return m_gizmo_hovered; }
    bool gizmo_active() const noexcept { return m_gizmo_using; }
    /// The latest inspector or gizmo rejection, shown under the edited component; empty when none.
    const std::string& edit_error() const noexcept { return m_edit_error; }
    /// The open project's collision group names, or the defaults without a project.
    CollisionGroupNames collision_groups() const;
    /// A note under a physics component in the Inspector: what Play will refuse (a warning), or which
    /// body a collider belongs to. Empty when there is nothing to say.
    struct PhysicsNote {
        std::string text;
        bool warning = false;
    };
    PhysicsNote physics_note(EntityId id, ComponentId component) const;
    /// What the script asset declares: its last good version's description, or why it has none (its
    /// error); null without a project.
    const ScriptDescription* script_description(AssetId script);
    /// Why the script file's current contents cannot be used (a compile error, or a missing file), or
    /// empty. With a last good version, that version stays in use.
    std::string script_error(AssetId script);
    /// Checks the project's script files for changes and reloads the changed ones, into play too. The
    /// editor checks every quarter second; this checks now.
    void check_script_files();
    /// Checks the files of loaded materials, textures, and environments, and the source images textures
    /// and environments name, for changes made outside the editor, and reloads the changed ones
    /// (docs/editor.md#watched-files). The editor checks every quarter second; this checks now.
    void check_asset_files();
    /// Checks imported glTF files, and the files they name, for changes made outside the editor, and
    /// imports a changed one again (docs/editor.md#watched-files). The editor checks every quarter
    /// second, before check_asset_files; this checks now.
    void check_imported_sources();
    /// Opens a script's file in the application the system uses for it.
    void open_script(AssetId script);
    /// While playing, the script VM's memory in use; 0 otherwise.
    size_t play_script_memory() const noexcept { return m_play_reloads ? m_play_reloads->memory() : 0; }
    /// Applies a gizmo's new world matrix to an entity as a validated local transform. Returns false
    /// (and changes nothing) when the parent cannot represent the pose, e.g. it would need shear.
    bool apply_world_matrix(EntityId id, const math::Affine& world);
    uint64_t frames() const noexcept { return m_frame; }
    /// The host's timing of the frame just submitted (Application::on_frame_timing), for the
    /// Performance section of Diagnostics.
    void record_frame(const FrameTiming& timing);
    /// Recent timings, in milliseconds, over the last Performance::window frames.
    struct Performance {
        static constexpr size_t window = 240;
        SampleWindow interval{window}, update{window}, wait{window}, render{window}, submit{window};
        SampleWindow extract{window}, view{window}, ui{window}; // parts of render
        SampleWindow gpu{window}; // GPU execution, as frames complete
        uint64_t gpu_frames = 0; // frames with a GPU time
        uint64_t gpu_dropped = 0;
        /// Per pass label: the GPU time of a frame's passes with that label, over the frames that had any.
        struct Pass {
            SampleWindow samples{window};
            uint64_t last_frame = 0; // gpu_frames when last seen
        };
        std::map<std::string, Pass> gpu_passes;
        uint64_t untimed_passes = 0;
    };
    const Performance& performance() const noexcept { return m_performance; }
    /// For inspection in tests; make it current only between frames.
    ImGuiContext* context() const noexcept { return m_context; }

    /// Reads preferences from `file`, and saves every change to them there. A missing file keeps the
    /// defaults; an unreadable one is reported and kept as it is until a change is saved.
    void use_preferences_file(std::filesystem::path file);
    const EditorPreferences& preferences() const noexcept { return m_preferences; }
    /// The viewport's physics debug views (docs/editor.md#physics-debug-views): while editing, from the
    /// collider components; while playing, from the play session's physics world. Saved as preferences.
    const PhysicsDebugOptions& physics_debug() const noexcept { return m_preferences.physics_debug; }
    void set_physics_debug(const PhysicsDebugOptions& options);
    /// The viewport's exposure view: the image, or its luminance or false-color exposure. Saved as a preference.
    DebugView debug_view() const noexcept { return m_preferences.debug_view; }
    void set_debug_view(DebugView view);
    bool skeletons() const noexcept { return m_preferences.skeletons; }
    void set_skeletons(bool on);
    /// While on, the primary selection's collider shows size and offset handles in place of the
    /// transform gizmo. Each drag is one undoable edit, validated as the Inspector's are.
    bool collider_editing() const noexcept { return m_collider_editing; }
    void set_collider_editing(bool on);
    /// The physics counts Diagnostics shows while playing (refreshed four times a second), else none.
    const std::optional<PhysicsStats>& shown_physics() const noexcept { return m_shown_physics; }
    const RendererStats& renderer_stats() const noexcept { return m_renderer.stats(); }
    /// The debug lines and outlines the last viewport render drew.
    const DebugDraw& viewport_debug() const noexcept { return m_debug_draw; }

private:
    void apply_input(const RoutedInput& routed);
    void rebuild_fonts(float scale);
    void build_dock_layout(unsigned int dockspace);
    void draw_hierarchy();
    void draw_component(EntityId id, const ComponentValue& value);
    bool edit_property(EntityId id, const ComponentValue& value, PropertyId property, PropertyValue input);
    void draw_property(const PropertyDescriptor& property, const PropertyValue& value, const std::string& key,
                       const std::string& group, EntityId owner, const std::function<void(PropertyValue)>& edit);
    void track_edit(const std::string& label);
    /// A material asset's properties, opening it for editing on first use (docs/editor.md#materials);
    /// `folded` starts it closed under a header that opens it.
    void draw_material(AssetId id, bool folded);
    bool edit_material(AssetId id, PropertyId property, PropertyValue input);
    /// Writes each edited material to its file. Returns why one could not be written, or empty.
    std::string save_materials();
    /// Puts the files' values back into the registry for materials edited but not saved, as the editor
    /// that holds the edits closes.
    void discard_material_edits();
    /// A watched file as last read, so the watcher does not take the editor's own saves or reloads
    /// for outside changes.
    void note_watched_file(AssetId id);
    void draw_viewport_tools(const RenderView& view, ImVec2 min, ImVec2 max);
    void draw_physics_debug_menu();
    /// Size and offset handles on the primary selection's collider. Returns whether they are shown.
    bool draw_collider_handles(const RenderView& view, ImVec2 min, ImVec2 max);
    void save_preferences();
    void pick_at(ImVec2 point, const RenderView& view, ImVec2 min, ImVec2 max);
    void frame_selection();
    /// Sets the camera's pivot and pan scale as orbit, pan, or zoom starts at a viewport point.
    void start_navigation(NavigationMode mode, ImVec2 point);
    void draw_hierarchy_row(EntityId id);
    void draw_create_menu(std::optional<EntityId> parent);
    void handle_shortcuts();
    void report(const EditResult& result, const std::string& action);
    void start_rename(EntityId id);
    void draw_viewport();
    void draw_inspector();
    void draw_assets();
    struct AssetRow {
        AssetRecord record;
        std::string search; // the lowercase path, for the filter
        bool missing = false; // the source file does not exist
    };
    void draw_asset_row(const AssetRow& row);
    /// A texture's thumbnail (or the placeholder's, when it failed), loading at most a few textures a
    /// frame; 0 while it waits to load.
    ImTextureID texture_thumbnail(AssetId texture, bool preview);
    void draw_scene_menu();
    void draw_prompts();
    void draw_play_controls();
    void draw_view_toggle();
    void update_play(const RoutedInput& routed, float delta_time);
    void accept_asset_drop(EntityId target);
    void accept_viewport_drop();
    std::optional<Ray> viewport_ray(ImVec2 point) const;
    std::optional<EntityId> mesh_at(ImVec2 point) const;
    std::string asset_name(AssetId asset) const; // the file name without extension
    void place_in_view(AssetId mesh);
    void assign_to_selection(AssetId asset);
    void notice(std::string title, std::string message);
    void ask_save_as();
    void save_or_ask();
    enum class Pending { none, open, create, close };
    void request(Pending action, std::filesystem::path path);
    void perform_pending();
    void replace_scene(std::unique_ptr<SceneEditor> scene, std::filesystem::path path);
    void draw_collision_groups();
    void draw_script_properties(EntityId id, const ComponentValue& value, const ScriptComponent& script);
    void draw_script_notes(EntityId id, const ComponentValue& value, const ScriptComponent& script);
    std::string read_catalog(const Project& project, std::unique_ptr<AssetRegistry>& registry);
    void scan_project();
    void read_part_labels();
    void accept_dropped_files();
    /// Copies a glTF file from outside the project into models/ (a .gltf with the files it names, in a
    /// folder of its own); null, with a notice, when it cannot.
    std::optional<std::filesystem::path> copy_into_project(const std::filesystem::path& source);
    void draw_diagnostics();
    void draw_top_bar();
    void draw_status_bar();
    void render_viewport();

    GraphicsDevice& m_device;
    PlatformServices m_services;
    std::string m_clipboard; // storage for ImGui's clipboard reads
    EditorFonts m_font_data; // the atlas borrows this data
    theme::Fonts m_fonts;
    ImGuiContext* m_context = nullptr;
    Renderer m_renderer;
    UiRenderer m_ui;
    TextureThumbnails m_thumbnails;
    std::unordered_map<AssetId, AssetRequest, PersistentIdHash> m_texture_loads; // thumbnails loading, reported when done
    RenderTarget m_viewport;
    ImTextureID m_viewport_texture = 0;
    std::unique_ptr<AssetRegistry> m_assets;
    std::unique_ptr<SceneEditor> m_scene;
    std::optional<EntityId> m_renaming;
    char m_rename_buffer[256] = {};
    bool m_rename_focus = false;
    // Inspector
    std::string m_edit_error;
    EntityId m_edit_error_entity{};
    std::string m_material_error;
    AssetId m_material_error_id{};
    // A material chosen in the Assets panel, shown until the scene selection changes.
    std::optional<AssetId> m_inspected_material;
    std::vector<EntityId> m_inspected_selection;
    EntityId m_euler_entity{}; // rotation shown as Euler angles, kept stable while dragging
    math::Vec3 m_euler{0.0f};
    bool m_edit_group_open = false;
    bool m_groups_open = false; // the Collision groups window
    std::array<std::array<char, 40>, collision_group_names> m_group_names{}; // its text fields
    std::string m_groups_error;
    /// A script as the editor knows it (script_files.cpp).
    struct ScriptVersion {
        std::optional<ScriptSource> good; // the last version that compiled
        ScriptDescription description; // good's, or the error while there is none
        std::string error; // why the file's current contents cannot be used
        bool present = false; // what the file looked like when last read
        std::filesystem::file_time_type stamp;
        uintmax_t size = 0;
    };
    ScriptVersion& script_version(AssetId script); // read on first use
    void read_script(AssetId script, ScriptVersion& version);
    void reload_script(AssetId script);
    ScriptLimits script_limits() const;
    std::unordered_map<AssetId, ScriptVersion, PersistentIdHash> m_scripts;
    float m_file_check_timer = 0.0f; // scripts and materials are checked every quarter second
    /// A watched file as last seen (watched_files.cpp), and why its contents cannot be used.
    struct FileStamp {
        bool present = false;
        std::filesystem::file_time_type time;
        uintmax_t size = 0;
        bool operator==(const FileStamp&) const = default;
    };
    struct WatchedFile {
        FileStamp file; // the asset's own file
        FileStamp source; // a texture's or environment's source image, as its file names it
        std::string error;
    };
    WatchedFile look_at(const AssetRecord& record) const;
    std::unordered_map<AssetId, WatchedFile, PersistentIdHash> m_watched_files;
    /// Reloads of changed files in flight, reported when they finish (finish_reloads, every frame).
    struct PendingReload {
        AssetRequest request;
        AssetKind kind;
        std::string previous_error;
    };
    std::unordered_map<AssetId, PendingReload, PersistentIdHash> m_reloads;
    void finish_reloads();
    AssetLoadStats m_load_stats;
    /// An imported glTF file (content-relative), and it and the files it names as last seen.
    struct ImportedSource {
        std::filesystem::path source;
        std::vector<std::filesystem::path> files; // the source first
        std::vector<FileStamp> stamps;
    };
    std::vector<ImportedSource> m_imported_sources; // read with the import files, at each scan
    std::vector<FileStamp> stamp_files(const std::vector<std::filesystem::path>& files) const;
    std::array<char, 256> m_script_text{}; // the script string property being typed into
    std::string m_script_text_key;
    char m_name_buffer[256] = {};
    EntityId m_name_entity{};
    // Viewport tools
    GizmoOperation m_gizmo = GizmoOperation::translate;
    bool m_gizmo_local = false;
    bool m_gizmo_using = false;
    bool m_gizmo_hovered = false;
    // Collider handles: a drag moves one handle along its line, from the collider as it was at the press.
    bool m_collider_editing = false;
    struct ColliderDrag {
        EntityId entity;
        std::string handle; // "collider.+x", "collider.centre", ...
        ColliderComponent start;
        float start_along = 0.0f; // where the pointer was on the handle's line, in entity-local metres
        math::Vec3 start_point{0.0f}; // centre drags: the pointer on the plane facing the camera
    };
    std::optional<ColliderDrag> m_collider_drag;
    bool m_handle_hovered = false;
    // Preferences and physics debug views.
    EditorPreferences m_preferences;
    std::filesystem::path m_preferences_file;
    DebugDraw m_debug_draw; // this frame's, reused
    SkinBindingCache m_skin_bindings; // the shown World's skins' joints, between frames (#1038)
    std::optional<PhysicsStats> m_shown_physics;
    float m_physics_age = 0.0f; // seconds since m_shown_physics was taken
    std::optional<RenderSnapshot> m_snapshot; // the last rendered frame, for picking and outlines
    ImVec2 m_pick_point{-1.0f, -1.0f};
    std::vector<EntityId> m_pick_hits;
    size_t m_pick_index = 0;
    std::optional<EntityId> m_reveal; // expand and scroll the hierarchy to this entity
    // Play
    std::unique_ptr<PlaySession> m_play;
    std::shared_ptr<ScriptReloads> m_play_reloads; // changed scripts for the play session
    std::optional<PlayRecording> m_play_recording; // the running Play's, finished at Stop
    std::optional<PlayRecording> m_last_recording;
    std::optional<uint64_t> m_replay_final; // the state a replay should end in
    bool begin_play(const SceneDocument& document, const PlayRecording* replay, bool record);
    std::vector<RecordedAsset> played_assets(const SceneDocument& document);
    std::vector<EntityId> m_play_selection; // the selection at Play, restored at Stop
    bool m_game_view = true;
    // Project and scene files
    std::optional<Project> m_project;
    std::filesystem::path m_scene_path;
    std::vector<std::filesystem::path> m_scene_files;
    std::vector<std::filesystem::path> m_model_files; // glTF files in the content root
    std::shared_ptr<CookCache> m_cook_cache; // the open project's .maya/cache
    std::unordered_map<AssetId, std::string, PersistentIdHash> m_part_labels; // parts of imported files, by their import files
    std::vector<std::string> m_dropped_files; // dropped on the window this frame
    std::vector<AssetId> m_missing_files; // catalog entries whose source file does not exist
    std::vector<AssetRow> m_asset_rows; // the catalog in the Assets panel, read on open and refresh
    std::array<std::vector<size_t>, 7> m_shown_rows; // scenes, meshes, materials, scripts, textures, environments, and skins and clips passing the filter
    std::string m_shown_filter;
    bool m_shown_stale = true; // the rows or scene files changed
    bool m_rescan = false; // recheck the rows once they are drawn
    Pending m_pending = Pending::none; // waits for the unsaved-changes prompt
    std::filesystem::path m_pending_path;
    bool m_close_confirmed = false;
    EditorPrompt m_prompt = EditorPrompt::none;
    bool m_prompt_opening = false; // the prompt's popup opens on the next frame
    std::string m_prompt_title;
    std::string m_prompt_message;
    bool m_prompt_caution = false; // the message is a warning, not an error
    char m_save_as_buffer[512] = {};
    std::filesystem::path m_replace_confirmed; // an existing file the person agreed to replace
    std::optional<AssetId> m_selected_asset;
    char m_asset_filter[128] = {};
    EditorCamera m_camera;
    float m_pivot_distance = 5.0f; // metres from the camera to the last pivot
    InputRouter m_router;
    std::optional<bool> m_capture_request;
    EditorLayout m_layout{};
    DiagnosticLog m_log;
    std::vector<RenderDiagnostic> m_frame_problems;
    RenderSnapshotStats m_extraction{};
    LightCounts m_lights{};
    PixelSize m_viewport_request{};
    float m_font_scale = 0.0f;
    RhiStats m_shown_stats{}; // refreshed a few times a second so the numbers stay readable
    Performance m_performance;
    struct ShownPerformance {
        Summary interval, update, wait, render, submit, extract, view, ui, gpu;
        std::vector<std::pair<std::string, Summary>> gpu_passes; // seen in the last window of frames, slowest first
        std::optional<size_t> gpu_reported;
        std::optional<ProcessMemory> process;
        AssetResidency assets;
    } m_shown_performance;
    float m_stats_age = 1.0f;
    int m_cursor = -1; // last ImGuiMouseCursor sent to the host
    bool m_layout_built = false;
    bool m_focus_viewport = false; // once, after the layout is built
    bool m_frame_ready = false;
    bool m_minimized = false;
    bool m_viewport_hovered = false;
    bool m_ui_wants_text = false;
    bool m_viewport_error = false;
    uint64_t m_frame = 0;
};

} // namespace maya::editor
