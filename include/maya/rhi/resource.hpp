#pragma once

#include <cstdint>

namespace maya {

using ResourceHandle = uint32_t;
const ResourceHandle INVALID_HANDLE = 0;

struct VertexBufferHandle { ResourceHandle handle = INVALID_HANDLE; };
struct IndexBufferHandle { ResourceHandle handle = INVALID_HANDLE; };
struct UniformBufferHandle { ResourceHandle handle = INVALID_HANDLE; };
struct TextureHandle { ResourceHandle handle = INVALID_HANDLE; };

} // namespace maya
