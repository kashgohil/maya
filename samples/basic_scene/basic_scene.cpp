#include "basic_scene.hpp"
#include "maya/core/camera.hpp"
#include "maya/core/file_system.hpp"
#include "maya/core/mesh.hpp"
#include "maya/assets/registry.hpp"
#include <fstream>
#include "maya/core/primitives.hpp"
#include "maya/core/scene.hpp"
#include "maya/core/texture.hpp"
#include <iostream>
#include <stdexcept>

namespace maya::samples {
namespace {

class BasicScene final : public Application {
public:
    bool on_start(GraphicsDevice& device) override {
        m_camera = std::make_unique<Camera>(60.0f, 1280.0f / 720.0f, 0.1f, 100.0f);
        m_camera->set_position({0.0f, 0.0f, 3.0f});
        const auto source = FileSystem::read_text("resources/shaders/metal/triangle.metal");
        if (source.empty()) return false;
        if (device.surface_format() == Format::undefined) {
            std::cerr << "[BasicScene] requires a presentable surface\n";
            return false;
        }
        auto pipeline = PipelineDesc{source, "vertexMain", "fragmentMain", {device.surface_format()},
            Format::depth32_float, {true, true, CompareFunction::less}, CullMode::back,
            Winding::counter_clockwise, "textured mesh"};
        const auto textured = device.create_pipeline(pipeline);
        pipeline.fragment_entry = "fragmentUnlit";
        pipeline.label = "unlit mesh";
        const auto unlit = device.create_pipeline(pipeline);
        for (const auto* created : {&textured, &unlit})
            if (!*created) { std::cerr << created->diagnostic.message << '\n'; return false; }

        const auto catalog_path = FileSystem::resolve("samples/basic_scene/assets/catalog.maya");
        if (!catalog_path) return false;
        auto catalog_file = std::ifstream(*catalog_path);
        const auto catalog = read_asset_catalog(catalog_file);
        if (!catalog) { std::cerr << catalog.diagnostic.message << '\n'; return false; }
        m_assets = std::make_unique<AssetRegistry>(catalog_path->parent_path(),
            std::make_unique<FileAssetProvider>(device));
        for (const auto& record : catalog.records) {
            const auto error = m_assets->register_asset(record);
            if (error) { std::cerr << error.message << '\n'; return false; }
        }
        auto pyramid = m_assets->acquire(AssetRef<MeshAsset>{{0x6d617961,1}});
        if (!pyramid) std::cerr << pyramid.diagnostic.message << '\n';
        auto cube = make_color_cube(device, 0.35f, {1.0f, 1.0f, 1.0f});
        if (!pyramid || !cube) return false;
        const uint32_t checkerboard[] = {0xFFFFFFFF, 0xFF000000, 0xFF000000, 0xFFFFFFFF};
        m_texture = std::make_unique<Texture>(device, checkerboard, 2, 2, "checkerboard");
        const auto sampler = device.create_sampler({});
        const auto uniforms = device.create_buffer({sizeof(SceneDrawUniforms), BufferUsage::uniform, "scene uniforms"});
        for (const auto* error : {&m_texture->error(), &sampler.diagnostic, &uniforms.diagnostic})
            if (*error) { std::cerr << error->message << '\n'; return false; }
        m_uniform_buffer = uniforms.handle;
        m_scene.add_object(std::move(pyramid.lease), Material{textured.handle, m_texture.get(), sampler.handle});
        m_scene.add_object(std::move(cube), Material{unlit.handle, nullptr, {}});
        std::cerr << "[BasicScene] loaded pyramid and cube\n";
        return true;
    }

    void on_update(float delta_time, bool input_enabled) override {
        if (input_enabled) m_camera->update(delta_time);
        m_rotation += 0.5f * delta_time;
        auto& objects = m_scene.objects();
        objects[0].model_matrix = math::Mat4::rotate_z(m_rotation)
            * math::Mat4::rotate_x(m_rotation * 0.5f);
        objects[1].model_matrix = math::Mat4::translate({-1.35f, 0.0f, 0.0f})
            * math::Mat4::rotate_y(m_rotation * 0.35f);
    }

    void on_render(GraphicsDevice& device) override {
        const auto surface = device.acquire_surface();
        if (!surface) return; // no drawable or zero-sized window: skip presentation this frame
        const auto& target = surface.target;
        const auto* depth = device.describe(m_depth);
        if (!depth || depth->width != target.width || depth->height != target.height) {
            device.destroy(m_depth); // retired after frames that use it complete
            auto created = device.create_texture({target.width, target.height, Format::depth32_float,
                TextureUsage::render_target, "scene depth"});
            if (!created) throw std::runtime_error(created.diagnostic.message);
            m_depth = created.handle;
        }
        auto pass = RenderPassDesc{};
        pass.colors.push_back({target.texture, LoadAction::clear, StoreAction::store, {0.1, 0.1, 0.1, 1.0}});
        pass.depth = DepthAttachment{m_depth, LoadAction::clear, StoreAction::dont_care, 1.0};
        pass.label = "basic scene";
        if (auto error = device.begin_render_pass(pass)) throw std::runtime_error(error.message);
        auto error = m_scene.render(device, m_uniform_buffer, m_camera->get_view_projection_matrix(),
            DirectionalLighting::default_sun(), m_camera->get_position());
        device.end_render_pass();
        if (error) throw std::runtime_error(error.message);
    }

    void on_resize(uint32_t width, uint32_t height) override {
        m_camera->set_aspect_ratio(static_cast<float>(width) / static_cast<float>(height));
    }

    void on_stop() noexcept override {
        m_scene = Scene{};
        m_assets.reset();
        m_texture.reset();
        m_camera.reset();
        // Device shutdown releases the remaining pipelines, sampler, uniform buffer, and depth target.
        m_uniform_buffer = {};
        m_depth = {};
        m_rotation = 0.0f;
    }

private:
    std::unique_ptr<AssetRegistry> m_assets;
    Scene m_scene;
    std::unique_ptr<Camera> m_camera;
    std::unique_ptr<Texture> m_texture;
    BufferHandle m_uniform_buffer;
    TextureHandle m_depth;
    float m_rotation = 0.0f;
};

} // namespace

std::unique_ptr<Application> create_basic_scene() {
    return std::make_unique<BasicScene>();
}

} // namespace maya::samples
