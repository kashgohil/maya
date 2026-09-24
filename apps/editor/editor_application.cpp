#include "editor_application.hpp"
#include "maya/rhi/graphics_device.hpp"

namespace maya::editor {
namespace {

// Editor-only startup boundary. Panels and scene authoring belong to later issues.
class EditorApplication final : public Application {
public:
    void on_render(GraphicsDevice& device) override {
        const auto surface = device.acquire_surface();
        if (!surface) return; // hidden, zero-sized, or no drawable this frame
        auto pass = RenderPassDesc{};
        pass.colors.push_back({surface.target.texture, LoadAction::clear, StoreAction::store, {0.1, 0.1, 0.1, 1.0}});
        pass.label = "editor background";
        if (!device.begin_render_pass(pass)) device.end_render_pass();
    }
};

} // namespace

std::unique_ptr<Application> create_editor_application() {
    return std::make_unique<EditorApplication>();
}

} // namespace maya::editor
