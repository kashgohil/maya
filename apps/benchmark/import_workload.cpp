// The import workload (#1036, docs/performance.md#import): each model is imported into a new project, then
// every part it cataloged is loaded twice, each time in a new session: cold, through an empty cook cache,
// which cooks and writes it; and warm, from the cache.

#include "benchmark_detail.hpp"
#include "maya/assets/cook_cache.hpp"
#include "maya/import/gltf_import.hpp"
#include <chrono>
#include <fstream>
#include <unistd.h>

namespace maya::benchmark {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
double since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

/// A project folder in the temporary directory, removed with it.
struct TemporaryProject {
    fs::path root;
    TemporaryProject(const std::string& name) {
        root = fs::temp_directory_path() / ("maya-benchmark-import-" + std::to_string(::getpid()) + "-" + name);
        fs::remove_all(root);
        fs::create_directories(root / "models");
        std::ofstream(root / "project.maya") << "maya-project 1\ncontent \".\"\ncatalog \"catalog.maya\"\n";
        std::ofstream(root / "catalog.maya") << "maya-assets 1\n";
    }
    ~TemporaryProject() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

/// Copies a glTF file, and the files it names, into the project's models folder.
fs::path copy_model(const fs::path& source, const fs::path& models) {
    auto files = std::vector<fs::path>{source.filename()};
    if (source.extension() == ".gltf") {
        const auto opened = GltfFile::open(source);
        if (!opened) throw std::runtime_error(source.string() + ": " + gltf_problem_text(opened.errors.front()));
        for (const auto& named : opened.file->document().files) files.emplace_back(named);
    }
    for (const auto& file : files) {
        fs::create_directories((models / file).parent_path());
        fs::copy_file(source.parent_path() / file, models / file, fs::copy_options::overwrite_existing);
    }
    return models / source.filename();
}

struct Loaded {
    double ms = 0.0;
    size_t triangles = 0, texture_gpu_bytes = 0;
};
/// Loads every part and material the import cataloged, in a new registry.
Loaded load_all(const Project& project, const std::vector<AssetRecord>& records, GraphicsDevice& device, std::shared_ptr<CookCache> cache) {
    const auto start = Clock::now();
    auto opened = open_project_assets(project, std::make_unique<FileAssetProvider>(device, std::move(cache)));
    if (!opened) throw std::runtime_error(opened.error);
    auto loaded = Loaded{};
    for (const auto& record : records) {
        auto failure = AssetDiagnostic{};
        if (record.kind == AssetKind::mesh) {
            const auto mesh = opened.registry->acquire(AssetRef<MeshAsset>{record.id});
            failure = mesh.diagnostic;
            if (mesh) loaded.triangles += mesh.lease.value().geometry().indices.size() / 3;
        } else if (record.kind == AssetKind::texture) {
            const auto texture = opened.registry->acquire(AssetRef<TextureAsset>{record.id});
            failure = texture.diagnostic;
            if (texture) loaded.texture_gpu_bytes += texture.lease.value().gpu_bytes();
        } else if (record.kind == AssetKind::material) {
            failure = opened.registry->acquire(AssetRef<MaterialAsset>{record.id}).diagnostic;
        }
        if (failure) throw std::runtime_error(failure.message);
    }
    loaded.ms = since(start);
    device.wait_idle(); // uploads retire before the next session
    return loaded;
}

/// The digest of every entry in a cache folder, in path order, and their total size.
std::pair<std::string, size_t> cache_contents(const fs::path& folder) {
    auto paths = std::vector<fs::path>{};
    for (const auto& entry : fs::recursive_directory_iterator(folder))
        if (entry.is_regular_file() && entry.path().filename() != ".gitignore") paths.push_back(entry.path());
    std::ranges::sort(paths);
    auto hasher = Sha256{};
    size_t bytes = 0;
    for (const auto& path : paths) {
        auto input = std::ifstream(path, std::ios::binary);
        const auto text = std::string(std::istreambuf_iterator<char>(input), {});
        hasher.update(path.lexically_relative(folder).generic_string());
        hasher.update(text);
        bytes += text.size();
    }
    return {sha256_text(hasher.finish()), bytes};
}
} // namespace

void detail::run_import(Result& result, const Manifest& manifest, GraphicsDevice& device) {
    for (const auto& model : manifest.models)
        if (!fs::is_regular_file(manifest.content / model))
            throw std::runtime_error("missing " + (manifest.content / model).string() + "; run tools/fetch_render_samples.sh");
    for (uint32_t run = 0; run < manifest.runs; ++run)
        for (const auto& model : manifest.models) {
            const auto folder = TemporaryProject(std::to_string(run) + "-" + model.stem().string());
            const auto copied = copy_model(manifest.content / model, folder.root / "models");
            auto opened = open_project(folder.root);
            if (!opened) throw std::runtime_error(opened.error);
            const auto& project = opened.project;

            auto sample = ImportSample{};
            sample.model = model.filename().string();
            auto start = Clock::now();
            const auto imported = import_gltf(project, copied);
            sample.import_ms = since(start);
            if (!imported) throw std::runtime_error(model.string() + ": " + gltf_problem_text(imported.errors.front()));
            for (const auto& record : imported.records) {
                sample.meshes += record.kind == AssetKind::mesh;
                sample.textures += record.kind == AssetKind::texture;
                sample.materials += record.kind == AssetKind::material;
            }
            sample.entities = imported.entities;

            const auto cache_folder = cook_cache_folder(project);
            const auto cold = load_all(project, imported.records, device, std::make_shared<CookCache>(cache_folder));
            sample.cold_ms = cold.ms;
            sample.triangles = cold.triangles;
            sample.texture_gpu_bytes = cold.texture_gpu_bytes;
            auto warm_cache = std::make_shared<CookCache>(cache_folder);
            sample.warm_ms = load_all(project, imported.records, device, warm_cache).ms;
            sample.warm_hits = warm_cache->stats().hits;
            sample.warm_misses = warm_cache->stats().misses;
            std::tie(sample.cache_digest, sample.cache_bytes) = cache_contents(cache_folder);
            result.imports.push_back(std::move(sample));
        }
    // Deterministic: each model's runs cooked the same bytes.
    auto same = true;
    for (const auto& sample : result.imports)
        for (const auto& other : result.imports)
            same = same && (other.model != sample.model || other.cache_digest == sample.cache_digest);
    result.deterministic = same;
    if (!same) result.failure = "runs cooked different bytes: a determinism failure";
    for (const auto& sample : result.imports)
        if (sample.warm_misses > 0 && result.failure.empty())
            result.failure = sample.model + ": " + std::to_string(sample.warm_misses) + " parts were not in the cook cache when loaded warm";
}
} // namespace maya::benchmark
