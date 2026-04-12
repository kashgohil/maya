#pragma once

#include "maya/rhi/resource.hpp"

namespace maya {

class Texture;

/// Minimal bind description: which pipeline to use and an optional base color texture
/// (textured fragment). If `texture` is null, only pipeline/uniforms are bound (unlit path).
struct Material {
    PipelineHandle pipeline{};
    Texture* texture = nullptr;
};

} // namespace maya
