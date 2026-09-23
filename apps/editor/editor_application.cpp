#include "editor_application.hpp"

namespace maya::editor {
namespace {

// Editor-only startup boundary. Panels and scene authoring belong to later issues.
class EditorApplication final : public Application {};

} // namespace

std::unique_ptr<Application> create_editor_application() {
    return std::make_unique<EditorApplication>();
}

} // namespace maya::editor
