#pragma once

#include "maya/rhi/resource.hpp"

namespace maya {

class Texture;

/// Minimal bind description: a pipeline plus an optional base color texture and its sampler.
/// If `texture` is null, only the pipeline and uniforms are bound (unlit path).
struct Material {
    PipelineHandle pipeline{};
    const Texture* texture = nullptr;
    SamplerHandle sampler{};
};

} // namespace maya
