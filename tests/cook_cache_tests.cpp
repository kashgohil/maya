// The cook cache (#1036, docs/assets.md#cook-cache): SHA-256 keys, entries that survive only intact,
// and loading through FileAssetProvider, which reads what it cooked before and gives the same result.

#include "maya/assets/cook_cache.hpp"
#include "maya/assets/environment_cook.hpp"
#include "maya/assets/registry.hpp"
#include "maya/assets/texture_cook.hpp"
#include "maya/import/gltf_import.hpp"
#include "maya/rhi/null_device.hpp"
#include "support/gltf.hpp"
#include "support/hdr.hpp"
#include "support/png.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

using namespace maya;
namespace fs = std::filesystem;

namespace {
struct Folder {
    Folder() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / ("maya-cook-cache-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(root);
    }
    ~Folder() {
        std::error_code ignored;
        fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ignored);
        fs::remove_all(root, ignored);
    }
    void write(const std::string& name, const std::string& text) const {
        fs::create_directories((root / name).parent_path());
        std::ofstream(root / name, std::ios::binary) << text;
    }
    fs::path root;
};
std::span<const std::byte> bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }
std::vector<std::byte> read_all(const fs::path& path) {
    auto input = std::ifstream(path, std::ios::binary);
    auto out = std::vector<std::byte>{};
    for (std::istreambuf_iterator<char> it(input), end; it != end; ++it) out.push_back(std::byte(*it));
    return out;
}
/// Every entry in the cache folder, by path.
std::map<std::string, std::vector<std::byte>> entries(const fs::path& folder) {
    auto found = std::map<std::string, std::vector<std::byte>>{};
    if (!fs::exists(folder)) return found;
    for (const auto& entry : fs::recursive_directory_iterator(folder))
        if (entry.is_regular_file() && entry.path().filename() != ".gitignore")
            found.emplace(entry.path().lexically_relative(folder).generic_string(), read_all(entry.path()));
    return found;
}
std::string image_bytes(uint32_t width, uint32_t height, uint8_t seed) {
    auto rgba = std::vector<uint8_t>{};
    for (uint32_t i = 0; i < width * height; ++i) rgba.insert(rgba.end(), {uint8_t(i * 7 + seed), uint8_t(i * 3), uint8_t(255 - i), 255});
    return test::encode_png_rgba(width, height, rgba);
}
} // namespace

TEST_CASE("SHA-256 gives FIPS 180-4's digests, however the data is split", "[assets][cache]") {
    CHECK(sha256_text(sha256(bytes(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_text(sha256(bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto two_blocks = std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    CHECK(sha256_text(sha256(bytes(two_blocks))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const auto million = std::string(1'000'000, 'a');
    CHECK(sha256_text(sha256(bytes(million))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    for (const size_t piece : {1u, 55u, 56u, 63u, 64u, 65u, 1000u}) {
        INFO(piece);
        auto hasher = Sha256{};
        for (size_t at = 0; at < million.size(); at += piece) hasher.update(std::string_view(million).substr(at, piece));
        CHECK(sha256_text(hasher.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
}

TEST_CASE("Cache entries read back only for the same key, and only when intact", "[assets][cache]") {
    const Folder folder;
    auto cache = CookCache(folder.root / ".maya/cache");
    const auto key = CookKey{"texture", 1, sha256(bytes("source")), "role color\n"};
    CHECK_FALSE(cache.read(key));
    const auto payload = std::string("cooked texels");
    cache.write(key, bytes(payload));
    CHECK(cache.stats().writes == 1);
    // A .gitignore keeps the folder out of version control.
    CHECK(fs::exists(folder.root / ".maya/cache/.gitignore"));
    CHECK(cache.entry_path(key).generic_string().ends_with(".texture"));
    const auto back = cache.read(key);
    REQUIRE(back);
    CHECK(std::string(reinterpret_cast<const char*>(back->data()), back->size()) == payload);
    CHECK(cache.stats().hits == 1);
    // Another source, settings, version, or kind is another entry.
    for (auto other : {CookKey{"texture", 1, sha256(bytes("source 2")), "role color\n"}, CookKey{"texture", 1, key.source, "role data\n"},
                       CookKey{"texture", 2, key.source, "role color\n"}, CookKey{"mesh", 1, key.source, "role color\n"}}) {
        CHECK(other.digest() != key.digest());
        CHECK_FALSE(cache.read(other));
    }
    // A damaged entry, truncated or with a changed byte, is not read.
    const auto path = cache.entry_path(key);
    auto stored = read_all(path);
    stored.back() ^= std::byte{1};
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(stored.data()), std::streamsize(stored.size()));
    CHECK_FALSE(cache.read(key));
    fs::resize_file(path, 20);
    CHECK_FALSE(cache.read(key));
    CHECK(cache.stats().damaged == 2);
    // Writing again replaces it.
    cache.write(key, bytes(payload));
    CHECK(cache.read(key));
}

TEST_CASE("A cache that cannot be written is counted, and nothing else changes", "[assets][cache]") {
    if (::geteuid() == 0) SKIP("permissions do not apply to root");
    const Folder folder;
    fs::create_directories(folder.root / "locked");
    fs::permissions(folder.root / "locked", fs::perms::owner_read | fs::perms::owner_exec);
    auto cache = CookCache(folder.root / "locked/cache");
    const auto key = CookKey{"texture", 1, {}, ""};
    cache.write(key, bytes("x"));
    CHECK(cache.stats().failures == 1);
    CHECK(cache.stats().writes == 0);
    CHECK_FALSE(cache.read(key));
    fs::permissions(folder.root / "locked", fs::perms::owner_all);
}

TEST_CASE("Textures, environments, and imported meshes and textures load from the cache as they were cooked", "[assets][cache]") {
    const Folder folder;
    // A texture, an environment, and an imported model.
    folder.write("wood.png", image_bytes(16, 8, 1));
    folder.write("wood.texture", "maya-texture 1\nsource \"wood.png\"\nusage color\ncompression astc\nmips on\n"
                                 "filter linear linear\nmip_filter linear\nanisotropy 8\naddress repeat repeat\n");
    folder.write("sky.hdr", test::radiance_file(test::environment_image(32, [](const math::Vec3& d) { return math::Vec3{1.0f + d.y}; })));
    folder.write("sky.environment", "maya-environment 1\nsource \"sky.hdr\"\nspecular_size 16\nsamples 16\n");
    folder.write("models/props.gltf", test::props_gltf());
    folder.write("models/textures/normal.png", test::flat_normal_png());
    folder.write("project.maya", "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\n");
    folder.write("catalog.maya", "maya-assets 1\ntexture 1 1 \"wood.texture\"\nenvironment 1 2 \"sky.environment\"\n");
    auto project = open_project(folder.root);
    REQUIRE(project);
    const auto imported = import_gltf(project.project, "models/props.gltf");
    REQUIRE(imported);
    const auto cache_folder = cook_cache_folder(project.project);
    CHECK(cache_folder == fs::canonical(folder.root) / ".maya/cache");

    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    struct Loaded {
        std::vector<size_t> mesh_indices;
        std::array<math::Vec3, 9> irradiance{};
        uint32_t texture_levels = 0, imported_levels = 0;
        CookCache::Stats stats;
    };
    const auto load_all = [&](std::shared_ptr<CookCache> cache) {
        auto opened = open_project_assets(project.project, std::make_unique<FileAssetProvider>(device, cache));
        REQUIRE(opened);
        auto& registry = *opened.registry;
        auto loaded = Loaded{};
        for (const auto& record : registry.records()) {
            INFO(record.path.generic_string());
            if (record.kind == AssetKind::mesh) {
                const auto mesh = registry.acquire(AssetRef<MeshAsset>{record.id});
                REQUIRE(mesh);
                loaded.mesh_indices.push_back(mesh.lease.value().geometry().indices.size());
            } else if (record.kind == AssetKind::texture) {
                const auto texture = registry.acquire(AssetRef<TextureAsset>{record.id});
                REQUIRE(texture);
                (record.path == "wood.texture" ? loaded.texture_levels : loaded.imported_levels) = texture.lease.value().texture().desc().mip_levels;
            } else if (record.kind == AssetKind::environment) {
                const auto environment = registry.acquire(AssetRef<EnvironmentAsset>{record.id});
                REQUIRE(environment);
                loaded.irradiance = environment.lease.value().irradiance();
            }
        }
        if (cache) loaded.stats = cache->stats();
        return loaded;
    };
    // Cold: everything cooked once, and written: a texture, an environment, 3 meshes, 2 imported textures.
    const auto uncached = load_all(nullptr);
    const auto cold = load_all(std::make_shared<CookCache>(cache_folder));
    CHECK(cold.stats.hits == 0);
    CHECK(cold.stats.writes == 7);
    const auto written = entries(cache_folder);
    CHECK(written.size() == 7);
    // Warm, in a new session: every one read back, and nothing written.
    const auto warm = load_all(std::make_shared<CookCache>(cache_folder));
    CHECK(warm.stats.hits == 7);
    CHECK(warm.stats.writes == 0);
    for (const auto& loaded : {cold, warm}) {
        CHECK(loaded.mesh_indices == uncached.mesh_indices);
        CHECK(loaded.texture_levels == uncached.texture_levels);
        CHECK(loaded.imported_levels == uncached.imported_levels);
        for (size_t i = 0; i < 9; ++i) CHECK((loaded.irradiance[i] - uncached.irradiance[i]).length() == 0.0f);
    }
    // Deterministic: cooking again from scratch writes the same bytes.
    fs::remove_all(cache_folder);
    load_all(std::make_shared<CookCache>(cache_folder));
    CHECK(entries(cache_folder) == written);

    // A changed source or setting is cooked again, beside the entries for what was there before.
    folder.write("wood.png", image_bytes(16, 8, 2));
    folder.write("models/props.gltf.import", [&] {
        auto input = std::ifstream(folder.root / "models/props.gltf.import");
        auto text = std::string(std::istreambuf_iterator<char>(input), {});
        return text.replace(text.find("compression astc"), 16, "compression rgba8");
    }());
    const auto changed = load_all(std::make_shared<CookCache>(cache_folder));
    CHECK(changed.stats.writes == 3); // the texture, and both imported textures
    CHECK(changed.stats.hits == 4); // the environment and the meshes
    CHECK(entries(cache_folder).size() == 10);
    // An image the glTF file names, beside it, is part of the source.
    folder.write("models/textures/normal.png", test::encode_png_rgba(1, 1, {128, 128, 255, 255}));
    const auto image_changed = load_all(std::make_shared<CookCache>(cache_folder));
    // A part's key covers every file the source names, so all 5 parts (3 meshes, 2 textures) are cooked again.
    CHECK(image_changed.stats.writes == 5);
    // Without an import file, what a glTF file names is unknown, so its parts are not cached.
    fs::remove(folder.root / "models/props.gltf.import");
    const auto unknown = load_all(std::make_shared<CookCache>(cache_folder));
    CHECK(unknown.stats.writes == 0);
    CHECK(unknown.stats.hits == 2); // the texture and the environment
    device.shutdown();
}
