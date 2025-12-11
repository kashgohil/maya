#pragma once

namespace maya {

enum class RendererAPI { None, Vulkan, OpenGL, Metal };

struct RendererConfig {
  RendererAPI api = RendererAPI::Vulkan;
  bool enable_vsync = true;
  bool enable_validation = true;
};

class Renderer {
public:
  static Renderer &instance();

  bool initialize(const RendererConfig &config);
  void shutdown();

  void begin_frame();
  void end_frame();

  RendererAPI get_api() const { return m_api; }

private:
  Renderer() = default;
  ~Renderer() = default;

  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;

  RendererAPI m_api = RendererAPI::None;
};

} // namespace maya
