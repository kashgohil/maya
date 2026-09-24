#pragma once

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

enum class Format : uint8_t {
    undefined, rgba8_unorm, rgba8_srgb, bgra8_unorm, bgra8_srgb, rgba16_float, depth32_float
};
constexpr bool is_depth_format(Format format) noexcept { return format == Format::depth32_float; }
constexpr bool is_color_format(Format format) noexcept {
    return format != Format::undefined && !is_depth_format(format);
}
constexpr uint32_t bytes_per_pixel(Format format) noexcept {
    switch (format) {
    case Format::undefined: return 0;
    case Format::rgba16_float: return 8;
    default: return 4;
    }
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
/// Two-dimensional, single mip level. Render targets and sampled textures live in GPU memory.
struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::undefined;
    TextureUsage usage = TextureUsage::none;
    std::string label;
};
enum class Filter : uint8_t { nearest, linear };
enum class AddressMode : uint8_t { repeat, clamp_to_edge, mirror_repeat };
struct SamplerDesc {
    Filter min_filter = Filter::linear;
    Filter mag_filter = Filter::linear;
    AddressMode address_u = AddressMode::repeat;
    AddressMode address_v = AddressMode::repeat;
    std::string label;
};

enum class CompareFunction : uint8_t { never, less, less_equal, equal, greater, greater_equal, always };
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
};

} // namespace maya
