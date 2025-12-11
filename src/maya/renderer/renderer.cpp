#include "maya/renderer/renderer.hpp"
#include <iostream>

namespace maya {

Renderer &Renderer::instance() {
  static Renderer s_instance;
  return s_instance;
}

bool Renderer::initialize(const RendererConfig &config) {
  std::cout << "[Renderer] Initializing renderer...\n";

  m_api = config.api;

  const char *api_name = "Unknown";
  switch (m_api) {
  case RendererAPI::Vulkan:
    api_name = "Vulkan";
    break;
  case RendererAPI::OpenGL:
    api_name = "OpenGL";
    break;
  case RendererAPI::Metal:
    api_name = "Metal";
    break;
  default:
    api_name = "None";
    break;
  }

  std::cout << "[Renderer] API: " << api_name << "\n";
  std::cout << "[Renderer] VSync: "
            << (config.enable_vsync ? "Enabled" : "Disabled") << "\n";
  std::cout << "[Renderer] Validation: "
            << (config.enable_validation ? "Enabled" : "Disabled") << "\n";

  std::cout << "[Renderer] Renderer initialized successfully\n";
  return true;
}

void Renderer::shutdown() {
  std::cout << "[Renderer] Shutting down renderer...\n";
  m_api = RendererAPI::None;
  std::cout << "[Renderer] Renderer shutdown complete\n";
}

void Renderer::begin_frame() {}

void Renderer::end_frame() {}

} // namespace maya
