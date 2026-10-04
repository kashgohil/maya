#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace maya {

/// Transient GPU resource identity: device session, slot, and slot generation. Never serialize.
template<class Tag>
struct RhiHandle {
    uint64_t session = 0;
    uint32_t slot = std::numeric_limits<uint32_t>::max();
    uint32_t generation = 0;
    constexpr bool valid() const noexcept { return session != 0; }
    auto operator<=>(const RhiHandle&) const = default;
};
using BufferHandle = RhiHandle<struct BufferTag>;
using TextureHandle = RhiHandle<struct TextureTag>;
using SamplerHandle = RhiHandle<struct SamplerTag>;
using PipelineHandle = RhiHandle<struct PipelineTag>;

/// ASTC formats are block-compressed (16 bytes per block of 4x4 or 6x6 texels) and can only be
/// sampled; they need RhiLimits::astc.
enum class Format : uint8_t {
    undefined, rgba8_unorm, rgba8_srgb, bgra8_unorm, bgra8_srgb, rgba16_float, depth32_float,
    astc_4x4_unorm, astc_4x4_srgb, astc_6x6_unorm, astc_6x6_srgb
};
inline constexpr Format last_format = Format::astc_6x6_srgb;
constexpr bool is_depth_format(Format format) noexcept { return format == Format::depth32_float; }
constexpr bool is_color_format(Format format) noexcept {
    return format != Format::undefined && !is_depth_format(format);
}
constexpr bool is_compressed_format(Format format) noexcept {
    return format >= Format::astc_4x4_unorm && format <= Format::astc_6x6_srgb;
}
/// Sampling decodes sRGB-encoded texels to linear values.
constexpr bool is_srgb_format(Format format) noexcept {
    return format == Format::rgba8_srgb || format == Format::bgra8_srgb || format == Format::astc_4x4_srgb ||
           format == Format::astc_6x6_srgb;
}
/// Texels per block side: 1 for uncompressed formats.
constexpr uint32_t block_extent(Format format) noexcept {
    switch (format) {
    case Format::astc_4x4_unorm: case Format::astc_4x4_srgb: return 4;
    case Format::astc_6x6_unorm: case Format::astc_6x6_srgb: return 6;
    default: return 1;
    }
}
/// Bytes per block (per pixel for uncompressed formats).
constexpr uint32_t block_bytes(Format format) noexcept {
    switch (format) {
    case Format::undefined: return 0;
    case Format::rgba16_float: return 8;
    case Format::astc_4x4_unorm: case Format::astc_4x4_srgb: case Format::astc_6x6_unorm: case Format::astc_6x6_srgb: return 16;
    default: return 4;
    }
}
/// Bytes per pixel of an uncompressed format; 0 for compressed formats (see block_bytes).
constexpr uint32_t bytes_per_pixel(Format format) noexcept {
    return is_compressed_format(format) ? 0 : block_bytes(format);
}
/// Size of mip level `level` of a width x height texture: max(1, size >> level).
constexpr uint32_t mip_extent(uint32_t size, uint32_t level) noexcept {
    return level >= 32 ? 1 : std::max<uint32_t>(1, size >> level);
}
/// Mip levels of a full chain down to 1x1.
constexpr uint32_t full_mip_count(uint32_t width, uint32_t height) noexcept {
    auto levels = uint32_t{1};
    for (auto size = std::max(width, height); size > 1; size >>= 1) ++levels;
    return levels;
}
/// Tightly packed bytes of one level: rows of blocks (or pixels), no padding.
constexpr size_t mip_level_bytes(Format format, uint32_t width, uint32_t height, uint32_t level) noexcept {
    const auto block = block_extent(format);
    const auto columns = (size_t{mip_extent(width, level)} + block - 1) / block;
    const auto rows = (size_t{mip_extent(height, level)} + block - 1) / block;
    return columns * rows * block_bytes(format);
}
const char* format_name(Format format) noexcept;

template<class E> requires std::is_enum_v<E>
constexpr E flags_or(E left, E right) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(left) | static_cast<U>(right));
}
template<class E> requires std::is_enum_v<E>
constexpr bool has_flag(E value, E flag) noexcept {
    using U = std::underlying_type_t<E>;
    return (static_cast<U>(value) & static_cast<U>(flag)) == static_cast<U>(flag) && static_cast<U>(flag) != 0;
}

enum class BufferUsage : uint32_t { none = 0, vertex = 1 << 0, index = 1 << 1, uniform = 1 << 2 };
constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept { return flags_or(a, b); }
enum class TextureUsage : uint32_t { none = 0, sampled = 1 << 0, render_target = 1 << 1, readback = 1 << 2 };
constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept { return flags_or(a, b); }

/// CPU-writable buffer. write_buffer is immediate and unsynchronized with in-flight GPU reads;
/// per-frame data belongs in GraphicsDevice::upload_transient.
struct BufferDesc {
    size_t size = 0;
    BufferUsage usage = BufferUsage::none;
    std::string label;
};
/// Two-dimensional. Render targets and sampled textures live in GPU memory. Textures with more than
/// one mip level, and compressed textures, can only be sampled.
/// A cube texture has six square faces per level, in the order +X, -X, +Y, -Y, +Z, -Z, and is
/// sampled by direction (since #1035).
enum class TextureType : uint8_t { texture_2d, cube };
struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::undefined;
    TextureUsage usage = TextureUsage::none;
    std::string label;
    uint32_t mip_levels = 1; // 1..full_mip_count(width, height)
    TextureType type = TextureType::texture_2d;
};
constexpr uint32_t texture_faces(const TextureDesc& desc) noexcept { return desc.type == TextureType::cube ? 6 : 1; }
/// Every level of a texture, tightly packed, level 0 first; within each level of a cube, its six faces in order.
constexpr size_t texture_bytes(const TextureDesc& desc) noexcept {
    auto total = size_t{0};
    for (uint32_t level = 0; level < desc.mip_levels; ++level) total += mip_level_bytes(desc.format, desc.width, desc.height, level);
    return total * texture_faces(desc);
}
enum class Filter : uint8_t { nearest, linear };
/// How samples combine mip levels: none reads level 0 only.
enum class MipFilter : uint8_t { none, nearest, linear };
enum class AddressMode : uint8_t { repeat, clamp_to_edge, mirror_repeat };
struct SamplerDesc {
    Filter min_filter = Filter::linear;
    Filter mag_filter = Filter::linear;
    AddressMode address_u = AddressMode::repeat;
    AddressMode address_v = AddressMode::repeat;
    std::string label;
    MipFilter mip_filter = MipFilter::none;
    uint32_t max_anisotropy = 1; // 1..16; 1 is off
};

enum class CompareFunction : uint8_t { never, less, less_equal, equal, greater, greater_equal, always };
/// Color blending for every attachment. `alpha` is source-over with straight (non-premultiplied)
/// alpha: rgb = src.rgb * src.a + dst.rgb * (1 - src.a), a = src.a + dst.a * (1 - src.a).
enum class BlendMode : uint8_t { opaque, alpha };
enum class CullMode : uint8_t { none, front, back };
enum class Winding : uint8_t { clockwise, counter_clockwise };
struct DepthState {
    bool test = false;
    bool write = false;
    CompareFunction compare = CompareFunction::less;
};
/// Shaders fetch vertices from bound buffers; there is no fixed-function vertex layout.
struct PipelineDesc {
    std::string shader_source; // Metal Shading Language for the Metal backend
    std::string vertex_entry = "vertexMain";
    std::string fragment_entry = "fragmentMain";
    std::vector<Format> color_formats; // must match the pass attachments in order
    Format depth_format = Format::undefined;
    DepthState depth{};
    CullMode cull = CullMode::back;
    Winding front_face = Winding::counter_clockwise;
    std::string label;
    BlendMode blend = BlendMode::opaque;
};

enum class LoadAction : uint8_t { load, clear, dont_care };
enum class StoreAction : uint8_t { store, dont_care };
struct ColorAttachment {
    TextureHandle texture;
    LoadAction load = LoadAction::clear;
    StoreAction store = StoreAction::store;
    std::array<double, 4> clear_color{0.0, 0.0, 0.0, 1.0};
};
struct DepthAttachment {
    TextureHandle texture;
    LoadAction load = LoadAction::clear;
    StoreAction store = StoreAction::dont_care;
    double clear_depth = 1.0;
};
struct RenderPassDesc {
    std::vector<ColorAttachment> colors;
    std::optional<DepthAttachment> depth;
    std::string label;
};
/// Pixels outside the rectangle are not drawn. Framebuffer pixels, origin at the top left.
struct ScissorRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};
enum class IndexType : uint8_t { uint16, uint32 };
constexpr size_t index_size(IndexType type) noexcept { return type == IndexType::uint16 ? 2 : 4; }

enum class RhiError {
    none, device_unavailable, invalid_descriptor, unsupported, out_of_memory, stale_handle,
    wrong_state, invalid_usage, out_of_range, misaligned, incompatible_pipeline,
    shader_compilation, surface_unavailable, gpu_failure, timeout
};
struct RhiDiagnostic {
    RhiError code = RhiError::none;
    std::string message;
    explicit operator bool() const noexcept { return code != RhiError::none; }
};
template<class Handle>
struct RhiResult {
    Handle handle;
    RhiDiagnostic diagnostic;
    explicit operator bool() const noexcept { return handle.valid(); }
};

/// The presentable texture for this frame. It is valid only until end_frame.
struct SurfaceTarget {
    TextureHandle texture;
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::undefined;
};
struct SurfaceResult {
    SurfaceTarget target;
    RhiDiagnostic diagnostic;
    explicit operator bool() const noexcept { return target.texture.valid(); }
};

/// Per-session frame policy, fixed at initialize.
struct DeviceOptions {
    /// Frames the CPU may encode ahead of GPU completion (1-8). begin_frame waits beyond this.
    uint32_t frames_in_flight = 3;
    /// Upload memory per frame slot for per-draw constants and dynamic data; 0 disables it.
    size_t transient_bytes_per_frame = size_t{4} << 20;
};
/// A range of one frame's upload memory. Valid for binding only during the frame that produced it.
struct TransientSlice {
    BufferHandle buffer;
    size_t offset = 0;
    size_t size = 0;
    uint64_t frame = 0;
};
struct TransientResult {
    TransientSlice slice;
    RhiDiagnostic diagnostic;
    explicit operator bool() const noexcept { return slice.size != 0; }
};

struct RhiLimits {
    uint32_t max_texture_dimension = 16384;
    uint32_t max_color_attachments = 8;
    uint32_t max_buffer_slots = 31; // vertex and uniform buffers share one table per stage
    uint32_t max_texture_slots = 31;
    uint32_t max_sampler_slots = 16;
    size_t max_buffer_size = size_t{256} << 20;
    size_t uniform_offset_alignment = 256;
    size_t vertex_offset_alignment = 4;
    uint32_t max_anisotropy = 16;
    bool astc = false; // ASTC LDR texture formats can be created and sampled
};
struct RhiStats {
    size_t buffers = 0; // excludes the device's own per-frame upload buffers
    size_t textures = 0; // excludes the transient surface texture
    size_t samplers = 0;
    size_t pipelines = 0;
    size_t pending_retirements = 0; // destroyed, waiting for GPU completion
    uint64_t submitted_frames = 0;
    uint64_t completed_frames = 0;
    uint64_t frame_waits = 0; // begin_frame calls that blocked on GPU completion
    uint64_t frame_wait_microseconds = 0;
    size_t transient_bytes_used = 0; // in the current or most recent frame
    size_t transient_high_water = 0;
    uint64_t transient_failures = 0; // uploads rejected because a frame's memory was exhausted
    // Encoded in the current or most recent frame.
    uint32_t frame_passes = 0;
    uint64_t frame_draws = 0;
    uint64_t frame_instances = 0; // summed over draws
    uint64_t frame_triangles = 0; // vertices or indices / 3, times instances
    // Tracked allocations: sizes from the descriptors of live resources (textures as texture_bytes:
    // every mip level, tightly packed), not memory the platform reports as resident. See reported_memory().
    size_t buffer_bytes = 0;
    size_t texture_bytes = 0;
    size_t pending_retirement_bytes = 0; // destroyed buffers and textures awaiting GPU completion
    size_t upload_bytes = 0; // the device's own per-frame upload memory, across all frames in flight
};

/// One render pass of a frame, from GPU timestamps at its stage boundaries (#1026). On Apple GPUs a
/// later pass's vertex stage can run during an earlier pass's fragment stage, so each stage is timed
/// on its own and a pass's time is their sum, not the span from its first to last sample.
struct GpuPassTiming {
    std::string label; // RenderPassDesc::label, or "pass N" (from 1) when it has none
    double start_ms = 0.0; // when its first stage began, from the frame's GPU start
    double end_ms = 0.0; // when its last stage ended
    double vertex_ms = 0.0; // the vertex stage's own interval; 0 when the stage did not run
    double fragment_ms = 0.0;
    double milliseconds() const noexcept { return vertex_ms + fragment_ms; }
};

/// How long the GPU spent executing one submitted frame, measured on the GPU's own timeline and
/// reported when the frame completes.
struct GpuFrameTiming {
    uint64_t frame = 0; // the submission serial (RhiStats::submitted_frames when it was submitted)
    double milliseconds = 0.0;
    double started = 0.0; // GPU clock seconds when the frame began executing; for ordering only
    /// Its passes in encoding order, when pass timing is supported and on (see
    /// GraphicsDevice::gpu_pass_timing_supported); empty otherwise.
    std::vector<GpuPassTiming> passes;
    uint32_t untimed_passes = 0; // passes beyond GraphicsDevice::max_timed_passes
};

/// When one presented frame reached the display (#1026).
struct PresentTiming {
    uint64_t frame = 0; // the submission serial
    std::optional<double> presented; // host clock seconds; nullopt when the frame was never shown
};

} // namespace maya
