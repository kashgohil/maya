#include "maya/import/gltf_import.hpp"
#include "maya/assets/material_file.hpp"
#include "maya/assets/property_context.hpp"
#include "maya/core/file_replace.hpp"
#include "maya/scene/scene_io.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace maya {
namespace {
namespace fs = std::filesystem;

/// A material slot's texture role: color for base color and emissive, normal for normals, data otherwise.
TextureRole slot_role(GltfMap map) {
    return map == GltfMap::base_color || map == GltfMap::emissive ? TextureRole::color
         : map == GltfMap::normal ? TextureRole::normal : TextureRole::data;
}
/// A name made safe for a file: letters, digits, spaces, and - _ . ( ), at most 64 bytes.
std::string file_name(const std::string& name) {
    auto safe = std::string{};
    for (const auto c : name)
        safe += std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')' ? c : '_';
    safe = safe.substr(0, 64);
    while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) safe.pop_back();
    while (!safe.empty() && (safe.front() == ' ' || safe.front() == '.')) safe.erase(safe.begin());
    return safe.empty() ? "material" : safe;
}
/// `base`, or `base ~2`, `base ~3`, ... : the first not yet taken, which is then taken.
std::string unique(const std::string& base, std::set<std::string>& taken) {
    auto name = base;
    for (int n = 2; taken.contains(name); ++n) name = base + " ~" + std::to_string(n);
    taken.insert(name);
    return name;
}
std::optional<std::string> read_text(const fs::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), {});
}
std::string lowercase(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

/// Gives IDs by identity: first the previous import's, then the catalog's for the same path (which keeps
/// a renamed part's ID: its old identity has gone, and its new one is where it was), then new ones. An ID
/// is never given twice. What changed since the previous import is reported as it goes.
template<class Id> class Identities {
public:
    template<class Entries> Identities(const Entries& previous, std::unordered_set<Id, PersistentIdHash> taken, std::string what,
                                       GltfImportResult& result)
        : m_taken(std::move(taken)), m_what(std::move(what)), m_result(result), m_reimport(!previous.empty()) {
        for (const auto& entry : previous) {
            m_previous.emplace(entry.identity, entry.id);
            m_previous_names.emplace(entry.id, entry.identity);
            m_previous_order.emplace_back(entry.identity, entry.id);
        }
    }
    /// Reports what the previous import had and this one does not; call after every `assign`.
    void finish() {
        for (const auto& [identity, id] : m_previous_order) // in the previous import's order
            if (!m_claimed.contains(id)) m_result.removed.push_back({m_what, identity, {}});
    }
    /// The previous import's ID for `identity`; call for every identity before `assign`.
    std::optional<Id> reuse(const std::string& identity) {
        const auto found = m_previous.find(identity);
        if (found == m_previous.end() || m_claimed.contains(found->second)) return std::nullopt;
        m_claimed.insert(found->second);
        return found->second;
    }
    /// For an identity `reuse` found nothing for: `candidate` (the catalog's ID for the same path) when no
    /// other identity has it, else a new ID.
    Id assign(const std::string& identity, std::optional<Id> candidate = std::nullopt) {
        if (candidate && !m_claimed.contains(*candidate)) {
            m_claimed.insert(*candidate);
            if (const auto before = m_previous_names.find(*candidate); before != m_previous_names.end())
                m_result.renamed.push_back({m_what, identity, before->second});
            else if (m_reimport) m_result.added.push_back({m_what, identity, {}});
            return *candidate;
        }
        auto id = Id::generate();
        while (!id.valid() || m_claimed.contains(id) || m_taken.contains(id)) id = Id::generate();
        m_claimed.insert(id);
        if (m_reimport) m_result.added.push_back({m_what, identity, {}});
        return id;
    }
private:
    std::unordered_map<std::string, Id> m_previous;
    std::unordered_map<Id, std::string, PersistentIdHash> m_previous_names;
    std::vector<std::pair<std::string, Id>> m_previous_order;
    std::unordered_set<Id, PersistentIdHash> m_claimed, m_taken;
    std::string m_what;
    GltfImportResult& m_result;
    bool m_reimport;
};

class NoLoads final : public AssetProvider {
public:
    AssetLoadResult<MeshAsset> load_mesh(const fs::path&) override { return {nullptr, {AssetError::load_failed, "not loaded"}}; }
    AssetLoadResult<MaterialAsset> load_material(const fs::path&) override { return {nullptr, {AssetError::load_failed, "not loaded"}}; }
};

struct Importer {
    const Project& project;
    fs::path absolute, source; // the glTF file; source is content-relative
    fs::path folder; // content-relative folder of the source
    const GltfDocument& document;
    GltfImportResult& result;
    ImportFile previous, next;
    std::vector<AssetRecord> catalog;
    std::map<std::string, AssetId> catalog_ids; // by generic path

    struct TextureUse {
        uint32_t texture;
        TextureRole role;
        AssetId id;
    };
    std::vector<TextureUse> textures;
    std::vector<std::vector<std::optional<AssetId>>> meshes; // by mesh, then primitive
    std::vector<AssetId> materials; // by glTF material, then the default material when it is used
    std::optional<AssetId> default_material;
    std::vector<AssetId> skins, animations; // by glTF skin and animation
    struct PendingFile {
        fs::path path; // content-relative
        std::string text;
    };
    std::vector<PendingFile> pending; // material files and the scene, in writing order

    std::optional<AssetId> catalog_id(const std::string& path) const {
        const auto found = catalog_ids.find(path);
        return found == catalog_ids.end() ? std::nullopt : std::optional(found->second);
    }
    std::unordered_set<AssetId, PersistentIdHash> catalog_set() const {
        auto ids = std::unordered_set<AssetId, PersistentIdHash>{};
        for (const auto& record : catalog) ids.insert(record.id);
        return ids;
    }
    std::string part_path(const std::string& part) const { return source.generic_string() + "#" + part; }

    void assign_meshes() {
        auto ids = Identities<AssetId>(previous.meshes, catalog_set(), "mesh", result);
        auto taken = std::set<std::string>{};
        struct Item { uint32_t mesh, primitive; std::string identity; std::optional<AssetId> id; };
        auto items = std::vector<Item>{};
        meshes.resize(document.meshes.size());
        for (uint32_t m = 0; m < document.meshes.size(); ++m) {
            const auto& mesh = document.meshes[m];
            meshes[m].resize(mesh.primitives.size());
            const auto base = mesh.name.empty() ? "#" + std::to_string(m) : mesh.name;
            for (uint32_t p = 0; p < mesh.primitives.size(); ++p)
                if (mesh.primitives[p].drawable) items.push_back({m, p, unique(base + "/" + std::to_string(p), taken), {}});
        }
        for (auto& item : items) item.id = ids.reuse(item.identity);
        for (auto& item : items) {
            const auto part = "mesh/" + std::to_string(item.mesh) + "/" + std::to_string(item.primitive);
            if (!item.id) item.id = ids.assign(item.identity, catalog_id(part_path(part)));
            meshes[item.mesh][item.primitive] = item.id;
            next.meshes.push_back({*item.id, part, item.identity});
            result.records.push_back({*item.id, AssetKind::mesh, part_path(part)});
        }
        ids.finish();
    }

    void assign_textures() {
        auto ids = Identities<AssetId>(previous.textures, catalog_set(), "texture", result);
        auto taken = std::set<std::string>{};
        struct Item { uint32_t texture; TextureRole role; std::string identity; std::optional<AssetId> id; };
        auto items = std::vector<Item>{};
        for (const auto& material : document.materials)
            for (size_t slot = 0; slot < material.maps.size(); ++slot) {
                const auto& map = material.maps[slot];
                if (!map || !document.textures[map->texture].image) continue;
                const auto role = slot_role(GltfMap(slot));
                if (std::ranges::any_of(items, [&](const Item& item) { return item.texture == map->texture && item.role == role; })) continue;
                const auto& texture = document.textures[map->texture];
                const auto& image = document.images[*texture.image];
                auto base = !texture.name.empty() ? texture.name : !image.name.empty() ? image.name
                          : !image.uri.empty() ? fs::path(image.uri).stem().string() : "#" + std::to_string(map->texture);
                items.push_back({map->texture, role, unique(base + "/" + texture_role_name(role), taken), {}});
            }
        for (auto& item : items) item.id = ids.reuse(item.identity);
        for (auto& item : items) {
            const auto part = "texture/" + std::to_string(item.texture) + "/" + texture_role_name(item.role);
            if (!item.id) item.id = ids.assign(item.identity, catalog_id(part_path(part)));
            textures.push_back({item.texture, item.role, *item.id});
            next.textures.push_back({*item.id, part, item.identity});
            result.records.push_back({*item.id, AssetKind::texture, part_path(part)});
        }
        ids.finish();
    }
    /// Skins and clips (#1038): one catalog part each, by name where the file has one.
    void assign_parts(AssetKind kind, size_t count, const auto& name_of, const std::vector<ImportedAsset>& before,
                      std::vector<ImportedAsset>& after, std::vector<AssetId>& assigned) {
        const auto what = std::string(asset_kind_name(kind));
        auto ids = Identities<AssetId>(before, catalog_set(), what, result);
        auto taken = std::set<std::string>{};
        auto identities = std::vector<std::string>{};
        auto found = std::vector<std::optional<AssetId>>{};
        for (size_t i = 0; i < count; ++i) {
            const auto& name = name_of(i);
            identities.push_back(unique(name.empty() ? "#" + std::to_string(i) : name, taken));
        }
        for (const auto& identity : identities) found.push_back(ids.reuse(identity));
        for (size_t i = 0; i < count; ++i) {
            const auto part = what + "/" + std::to_string(i);
            const auto id = found[i] ? *found[i] : ids.assign(identities[i], catalog_id(part_path(part)));
            assigned.push_back(id);
            after.push_back({id, part, identities[i]});
            result.records.push_back({id, kind, part_path(part)});
        }
        ids.finish();
    }
    void assign_skins() {
        assign_parts(AssetKind::skin, document.skins.size(), [&](size_t i) -> const std::string& { return document.skins[i].name; },
                     previous.skins, next.skins, skins);
    }
    void assign_animations() {
        assign_parts(AssetKind::animation, document.animations.size(),
                     [&](size_t i) -> const std::string& { return document.animations[i].name; }, previous.animations, next.animations,
                     animations);
    }
    std::optional<AssetId> texture_id(const std::optional<GltfTextureUse>& map, GltfMap slot) const {
        if (!map) return std::nullopt;
        const auto role = slot_role(slot);
        const auto found = std::ranges::find_if(textures, [&](const TextureUse& use) { return use.texture == map->texture && use.role == role; });
        return found == textures.end() ? std::nullopt : std::optional(found->id);
    }

    /// The material asset a glTF material imports as: its factors, its maps, and one texture transform.
    MaterialAsset material_value(const GltfMaterial& source, size_t index) {
        auto material = source.factors;
        const auto ref = [&](GltfMap slot) {
            const auto id = texture_id(source.maps[size_t(slot)], slot);
            return id ? AssetRef<TextureAsset>{*id} : AssetRef<TextureAsset>{};
        };
        material.base_color_texture = ref(GltfMap::base_color);
        material.metallic_roughness_texture = ref(GltfMap::metallic_roughness);
        material.normal_texture = ref(GltfMap::normal);
        material.occlusion_texture = ref(GltfMap::occlusion);
        material.emissive_texture = ref(GltfMap::emissive);
        // Maya has one transform per material: the base color map's, or the first map's that has one.
        const GltfTextureUse* chosen = nullptr;
        for (const auto& map : source.maps)
            if (map && (!chosen || (!chosen->transformed && map->transformed))) chosen = &*map;
        if (chosen && chosen->transformed) {
            material.uv_offset = chosen->offset;
            material.uv_rotation = chosen->rotation;
            material.uv_scale = chosen->scale;
        }
        const auto same = [&](const GltfTextureUse& map) {
            return map.offset.x == material.uv_offset.x && map.offset.y == material.uv_offset.y && map.rotation == material.uv_rotation &&
                   map.scale.x == material.uv_scale.x && map.scale.y == material.uv_scale.y;
        };
        if (std::ranges::any_of(source.maps, [&](const auto& map) { return map && !same(*map); }))
            result.warnings.push_back({"materials[" + std::to_string(index) + "]",
                "transforms its maps differently; Maya gives every map of a material one transform, so all use the base color map's"});
        return material;
    }

    /// Queues `text` for `path` (relative to the source's folder) unless the file there was edited since
    /// `written` was written: returns the hash recorded for the file.
    uint64_t queue(const fs::path& relative, const std::string& text, std::optional<uint64_t> written) {
        const auto content = folder / relative;
        const auto current = read_text(project.content_root / content);
        if (current && written && import_text_hash(*current) != *written) {
            result.kept.push_back(content);
            return *written;
        }
        if (!current || *current != text) pending.push_back({content, text});
        return import_text_hash(text);
    }
    /// A file name in `directory` (relative to the source's folder) that no file, catalog entry, or
    /// earlier choice uses.
    fs::path fresh_file(const fs::path& directory, const std::string& stem, const std::string& extension, std::set<std::string>& chosen) const {
        for (int n = 1;; ++n) {
            const auto name = directory / (stem + (n == 1 ? "" : " " + std::to_string(n)) + extension);
            const auto content = (folder / name).lexically_normal().generic_string();
            std::error_code ignored;
            if (!chosen.contains(lowercase(content)) && !catalog_ids.contains(content) && !fs::exists(project.content_root / content, ignored)) {
                chosen.insert(lowercase(content));
                return name;
            }
        }
    }

    void assign_materials() {
        auto ids = Identities<AssetId>(previous.materials, catalog_set(), "material", result);
        auto taken = std::set<std::string>{};
        struct Item { std::optional<size_t> index; std::string identity, name; std::optional<AssetId> id; const ImportedMaterial* before = nullptr; };
        auto items = std::vector<Item>{};
        for (size_t i = 0; i < document.materials.size(); ++i) {
            const auto& name = document.materials[i].name;
            items.push_back({i, unique(name.empty() ? "#" + std::to_string(i) : name, taken), name.empty() ? "material " + std::to_string(i) : name, {}});
        }
        const auto uses_default = std::ranges::any_of(document.meshes, [](const GltfMesh& mesh) {
            return std::ranges::any_of(mesh.primitives, [](const GltfPrimitive& p) { return p.drawable && !p.material; });
        });
        if (uses_default) items.push_back({std::nullopt, unique("(default)", taken), "default", {}});
        for (auto& item : items) {
            item.id = ids.reuse(item.identity);
            const auto found = std::ranges::find(previous.materials, item.identity, &ImportedMaterial::identity);
            if (found != previous.materials.end()) item.before = &*found;
        }
        auto chosen = std::set<std::string>{};
        for (const auto& item : items)
            if (item.before) chosen.insert(lowercase((folder / item.before->file).lexically_normal().generic_string()));
        const auto stem = absolute.stem().string();
        for (auto& item : items) {
            const auto file = item.before ? item.before->file : fresh_file(fs::path(stem) / "materials", file_name(item.name), ".material", chosen);
            const auto path = (folder / file).lexically_normal();
            if (!item.id) item.id = ids.assign(item.identity, catalog_id(path.generic_string()));
            // glTF's default material is white, fully metallic, and fully rough.
            const auto value = item.index ? material_value(document.materials[*item.index], *item.index) : MaterialAsset{{1, 1, 1, 1}, 1, 1};
            const auto written = queue(file, write_material_file(value), item.before ? std::optional(item.before->written) : std::nullopt);
            next.materials.push_back({*item.id, file, item.identity, written});
            result.records.push_back({*item.id, AssetKind::material, path});
            if (item.index) materials.push_back(*item.id);
            else default_material = *item.id;
        }
        ids.finish();
    }

    SceneDocument scene() {
        auto ids = Identities<EntityId>(previous.entities, {}, "entity", result);
        auto document_out = SceneDocument{};
        struct Pending { std::string identity; std::optional<size_t> parent; std::vector<ComponentValue> components; };
        auto entities = std::vector<Pending>{};
        const auto add = [&](const std::string& identity, std::optional<size_t> parent, std::vector<ComponentValue> components) {
            entities.push_back({identity, parent, std::move(components)});
            return entities.size() - 1;
        };
        // The root carries the import's scale and axis conversion, so the whole hierarchy follows them.
        auto root_transform = TransformComponent{};
        root_transform.scale = math::Vec3{next.settings.scale};
        if (next.settings.up == ImportUp::z) root_transform.rotation = math::Quat::from_axis_angle({1, 0, 0}, -math::PI / 2);
        auto root_components = std::vector<ComponentValue>{NameComponent{absolute.stem().string()}, root_transform};
        // The file's first clip plays on the whole import; the others are in the catalog to choose from.
        if (!animations.empty()) root_components.push_back(AnimationComponent{{animations.front()}});
        const auto root = add("/", std::nullopt, std::move(root_components));
        const auto renderer = [&](uint32_t mesh, uint32_t primitive) {
            const auto& source = document.meshes[mesh].primitives[primitive];
            const auto material = source.material ? materials[*source.material] : *default_material;
            return MeshRendererComponent{{*meshes[mesh][primitive]}, {material}, true};
        };
        const auto names = gltf_node_names(document); // as skins and clips name joints (#1038)
        const auto visit = [&](auto&& self, uint32_t index, size_t parent, const std::string& parent_identity, std::set<std::string>& siblings) -> void {
            const auto& node = document.nodes[index];
            const auto segment = [](std::string name) { std::ranges::replace(name, '/', '_'); return name; };
            const auto identity = (parent_identity == "/" ? "" : parent_identity) + "/" +
                                  unique(segment(node.name.empty() ? "node " + std::to_string(index) : node.name), siblings);
            auto components = std::vector<ComponentValue>{NameComponent{names[index]}, node.transform};
            auto drawn = std::vector<uint32_t>{};
            if (node.mesh)
                for (uint32_t p = 0; p < document.meshes[*node.mesh].primitives.size(); ++p)
                    if (document.meshes[*node.mesh].primitives[p].drawable) drawn.push_back(p);
            // A skinned mesh's entities name its skin; its joints are entities of the import too.
            const auto skin = node.skin ? std::optional(SkinComponent{{skins[*node.skin]}}) : std::nullopt;
            if (drawn.size() == 1) components.push_back(renderer(*node.mesh, drawn[0]));
            if (drawn.size() == 1 && skin) components.push_back(*skin);
            if (node.camera && next.settings.cameras) components.push_back(document.cameras[*node.camera].camera);
            if (node.light && next.settings.lights) components.push_back(document.lights[*node.light].light);
            const auto self_index = add(identity, parent, std::move(components));
            auto children = std::set<std::string>{};
            // A mesh with several primitives: a child entity for each, as Maya draws one material per mesh.
            if (drawn.size() > 1)
                for (const auto p : drawn) {
                    const auto& primitive = document.meshes[*node.mesh].primitives[p];
                    const auto label = primitive.material && !document.materials[*primitive.material].name.empty()
                                     ? document.materials[*primitive.material].name : "Primitive " + std::to_string(p);
                    auto parts = std::vector<ComponentValue>{NameComponent{label}, TransformComponent{}, renderer(*node.mesh, p)};
                    if (skin) parts.push_back(*skin);
                    add(identity + "/" + unique("primitive " + std::to_string(p), children), self_index, std::move(parts));
                }
            for (const auto child : node.children) self(self, child, self_index, identity, children);
        };
        auto roots = std::set<std::string>{};
        for (const auto index : document.roots) visit(visit, index, root, "/", roots);

        // IDs: the previous import's by node path, then new ones; parents are then rewritten to them.
        auto assigned = std::vector<std::optional<EntityId>>(entities.size());
        for (size_t i = 0; i < entities.size(); ++i) assigned[i] = ids.reuse(entities[i].identity);
        for (size_t i = 0; i < entities.size(); ++i) {
            if (!assigned[i]) assigned[i] = ids.assign(entities[i].identity);
            next.entities.push_back({*assigned[i], entities[i].identity});
        }
        ids.finish();
        for (size_t i = 0; i < entities.size(); ++i) {
            const auto parent = entities[i].parent ? assigned[*entities[i].parent] : std::nullopt;
            document_out.entities.push_back({*assigned[i], parent, std::move(entities[i].components)});
        }
        result.entities = document_out.entities.size();
        return document_out;
    }
};
} // namespace

GltfImportResult import_gltf(const Project& project, const fs::path& given) {
    auto result = GltfImportResult{};
    const auto fail = [&](std::string path, std::string message) {
        result.errors.push_back({std::move(path), std::move(message)});
        return std::move(result);
    };
    const auto absolute = project.resolve(given);
    if (!absolute) return fail("", given.generic_string() + " is outside the project's content");
    const auto source = project.relative(*absolute);
    const auto extension = lowercase(absolute->extension().string());
    if (extension != ".gltf" && extension != ".glb") return fail("", source.generic_string() + " is not a .gltf or .glb file");
    std::error_code error;
    if (!fs::is_regular_file(*absolute, error)) return fail("", source.generic_string() + " is missing");
    auto opened = GltfFile::open(*absolute);
    if (!opened) {
        result.errors = std::move(opened.errors);
        return result;
    }
    const auto& document = opened.file->document();
    result.warnings = document.warnings;

    auto importer = Importer{.project = project, .absolute = *absolute, .source = source, .folder = source.parent_path(),
                             .document = document, .result = result};
    const auto import_path = import_file_path(*absolute);
    const auto import_name = import_file_path(source).generic_string();
    if (const auto text = read_text(import_path)) {
        auto input = std::istringstream(*text);
        auto read = read_import_file(input);
        if (!read) return fail("", import_name + ": " + read.error + "; fix the file or delete it to import afresh");
        importer.previous = std::move(read.file);
    }
    importer.next.settings = importer.previous.settings;
    // Files outside the source's folder cannot be read (GltfFile::image refuses them), so only those inside are listed.
    for (const auto& named : document.files)
        if (const auto path = fs::path(named).lexically_normal(); !path.is_absolute() && !path.empty() && *path.begin() != ".." &&
                                                                  named.find("://") == std::string::npos)
            importer.next.files.push_back(path);
    if (auto input = std::ifstream(project.catalog)) {
        auto read = read_asset_catalog(input);
        if (!read) return fail("", project.relative(project.catalog).generic_string() + ": " + read.diagnostic.message);
        importer.catalog = std::move(read.records);
    }
    for (const auto& record : importer.catalog) importer.catalog_ids.emplace(record.path.lexically_normal().generic_string(), record.id);

    importer.assign_meshes();
    importer.assign_textures();
    importer.assign_materials();
    importer.assign_skins();
    importer.assign_animations();
    auto scene = importer.scene();

    // The new catalog: this source's parts are replaced by the import's, and its materials added or kept.
    auto ours = std::unordered_set<AssetId, PersistentIdHash>{};
    for (const auto& record : result.records) ours.insert(record.id);
    auto catalog = std::vector<AssetRecord>{};
    for (const auto& record : importer.catalog) {
        const auto split = split_asset_path(record.path);
        if (ours.contains(record.id) || (!split.part.empty() && split.file.lexically_normal() == source.lexically_normal())) continue;
        catalog.push_back(record);
    }
    catalog.insert(catalog.end(), result.records.begin(), result.records.end());
    {
        auto check = AssetRegistry(project.content_root, std::make_unique<NoLoads>());
        for (const auto& record : catalog)
            if (const auto problem = check.register_asset(record)) return fail("", "the catalog would be invalid: " + problem.message);
    }
    const auto kinds = [&](AssetId id, ReferenceKind kind) {
        const auto found = std::ranges::find(catalog, id, &AssetRecord::id);
        if (found == catalog.end()) return ReferenceStatus::missing;
        return found->kind == reference_asset_kind(kind) ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
    };
    auto scene_text = std::ostringstream{};
    if (const auto problems = write_scene(scene_text, std::move(scene), PropertyValidationContext{kinds}); !problems.empty())
        return fail("", "the scene could not be written: " + problems.front().message);

    // The scene: the previous import's file, or `<stem>.scene` (numbered when that name is taken).
    auto chosen = std::set<std::string>{};
    const auto& before = importer.previous;
    const auto scene_file = !before.scene.empty() ? before.scene : importer.fresh_file({}, absolute->stem().string(), ".scene", chosen);
    importer.next.scene = scene_file;
    importer.next.scene_written = importer.queue(scene_file, scene_text.str(),
                                                 before.scene.empty() ? std::nullopt : std::optional(before.scene_written));
    result.scene = (importer.folder / scene_file).lexically_normal();

    // Writing: material files and the scene, then the import file, then the catalog. A failure stops
    // there, so the catalog never names what was not written.
    for (const auto& file : importer.pending) {
        const auto path = project.content_root / file.path;
        fs::create_directories(path.parent_path(), error);
        if (const auto failed = replace_file(path, file.text, file.path.extension() == ".scene" ? "scene" : "material"); !failed.empty())
            return fail("", file.path.generic_string() + ": " + failed);
        result.written.push_back(file.path);
    }
    if (const auto failed = replace_file(import_path, write_import_file(importer.next), "import file"); !failed.empty())
        return fail("", import_name + ": " + failed);
    result.written.push_back(import_file_path(source));
    auto catalog_text = std::ostringstream{};
    write_asset_catalog(catalog_text, catalog);
    if (const auto failed = replace_file(project.catalog, catalog_text.str(), "catalog"); !failed.empty())
        return fail("", project.relative(project.catalog).generic_string() + ": " + failed);
    result.written.push_back(project.relative(project.catalog));
    return result;
}
} // namespace maya
