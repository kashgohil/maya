#pragma once

#include "maya/rhi/resource.hpp"
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace maya {

struct GraphicsResourceLifetime {};

/// Completion state shared with backend callbacks that may outlive a frame call.
/// `completed` is a watermark: every frame up to it has finished, even if callbacks arrive out of order.
struct RhiCompletion {
    std::atomic<uint64_t> completed{0};
    std::mutex mutex;
    std::vector<uint64_t> early; // finished frames above the watermark
    std::vector<RhiDiagnostic> errors;
    static constexpr size_t timing_capacity = 1024; // older timings are dropped if nobody takes them
    std::vector<GpuFrameTiming> timings;
    uint64_t dropped_timings = 0;
    std::vector<PresentTiming> presents; // bounded by timing_capacity too
    uint64_t dropped_presents = 0;
    /// One frame finished executing.
    void complete(uint64_t serial) noexcept;
    /// Every frame up to `serial` finished (e.g. after waiting for the queue to drain).
    void complete_through(uint64_t serial) noexcept;
    void report(std::string message) noexcept;
    /// A frame's GPU execution time and start (GPU clock seconds), from any thread.
    void record_timing(uint64_t serial, double milliseconds, double started = 0.0) noexcept;
    /// When a presented frame was shown (host clock seconds), or that it never was, from any thread.
    void record_present(uint64_t serial, std::optional<double> presented) noexcept;
};

/// Single-owner-thread graphics device. Public calls validate handles, descriptors, usage,
/// offsets, and encoder state before reaching the backend, so every backend reports errors alike.
///
/// Lifecycle: initialize -> [begin_frame -> (acquire_surface) -> passes -> end_frame]* -> shutdown.
/// Destroyed resources are invalid immediately; their native objects are released only after
/// every frame that could reference them has completed on the GPU.
class GraphicsDevice {
public:
    GraphicsDevice() = default;
    /// Derived classes must call shutdown() in their own destructor.
    virtual ~GraphicsDevice() = default;
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    /// Starts a new session; any previous session is shut down first. Null window = headless.
    /// Invalid options (frames_in_flight outside 1-8, oversized upload memory) fail initialization.
    bool initialize(void* native_window_handle, const DeviceOptions& options = {});
    /// Idempotent and nonthrowing. Abandons an open frame, waits for submitted work, releases every
    /// resource, and invalidates all handles and the resource lifetime. The native window must be alive.
    void shutdown() noexcept;
    bool initialized() const noexcept { return m_session != 0; }
    uint64_t session() const noexcept { return m_session; }
    /// Borrowed resources must stop using this device when its session expires.
    std::weak_ptr<const GraphicsResourceLifetime> resource_lifetime() const noexcept { return m_resource_lifetime; }

    /// Surface backing size in framebuffer pixels. Zero sizes are ignored; takes effect on next acquire.
    void resize(uint32_t width, uint32_t height);
    /// Presentable surface format, or undefined for a headless session.
    Format surface_format() const noexcept { return m_surface_format; }
    const RhiLimits& limits() const noexcept { return m_limits; }
    const DeviceOptions& options() const noexcept { return m_options; }
    RhiStats stats() const noexcept;

    RhiResult<BufferHandle> create_buffer(const BufferDesc& desc, const void* initial_data = nullptr);
    /// Initial data is every mip level, level 0 first, each tightly packed rows of pixels (or of
    /// blocks for compressed formats): texture_bytes(desc) in all.
    RhiResult<TextureHandle> create_texture(const TextureDesc& desc, const void* initial_data = nullptr);
    /// As above, refusing data whose size is not texture_bytes(desc). Empty data creates an
    /// uninitialized texture.
    RhiResult<TextureHandle> create_texture(const TextureDesc& desc, std::span<const std::byte> initial_data);
    RhiResult<SamplerHandle> create_sampler(const SamplerDesc& desc);
    RhiResult<PipelineHandle> create_pipeline(const PipelineDesc& desc);
    /// Immediate CPU write into a buffer the caller owns. The caller must not overwrite ranges that a
    /// submitted frame still reads; per-frame data belongs in upload_transient instead.
    RhiDiagnostic write_buffer(BufferHandle buffer, size_t offset, const void* data, size_t size);
    /// Copies data into this frame's upload memory, which the GPU no longer reads from any earlier
    /// frame. Alignment 0 means limits().uniform_offset_alignment. Exhaustion returns out_of_memory
    /// and leaves the frame usable; the memory is recycled once this frame completes.
    TransientResult upload_transient(const void* data, size_t size, size_t alignment = 0);
    /// Synchronous copy of a readback-usage texture, outside a frame. Waits for earlier GPU work.
    RhiDiagnostic read_texture(TextureHandle texture, std::vector<std::byte>& pixels);
    const BufferDesc* describe(BufferHandle buffer) const noexcept;
    const TextureDesc* describe(TextureHandle texture) const noexcept;

    /// Invalidate the handle now and retire the native object after GPU completion.
    /// Stale handles and the transient surface texture are ignored and return false.
    bool destroy(BufferHandle handle) noexcept;
    bool destroy(TextureHandle handle) noexcept;
    bool destroy(SamplerHandle handle) noexcept;
    bool destroy(PipelineHandle handle) noexcept;

    /// Waits (reported in stats) until at most frames_in_flight - 1 frames are still executing.
    RhiDiagnostic begin_frame();
    /// At most one surface per frame; repeated calls return the same target. A missing drawable,
    /// zero-sized or headless surface returns a diagnostic; offscreen passes still work.
    SurfaceResult acquire_surface();
    RhiDiagnostic begin_render_pass(const RenderPassDesc& desc);
    RhiDiagnostic set_pipeline(PipelineHandle pipeline);
    /// Buffer indices share one table with uniform buffers. Vertex buffers bind to the vertex stage.
    RhiDiagnostic set_vertex_buffer(uint32_t index, BufferHandle buffer, size_t offset = 0);
    RhiDiagnostic set_vertex_buffer(uint32_t index, const TransientSlice& slice);
    /// Binds to vertex and fragment stages. Offset must be a multiple of limits().uniform_offset_alignment.
    RhiDiagnostic set_uniform_buffer(uint32_t index, BufferHandle buffer, size_t offset = 0);
    RhiDiagnostic set_uniform_buffer(uint32_t index, const TransientSlice& slice);
    RhiDiagnostic set_texture(uint32_t index, TextureHandle texture);
    RhiDiagnostic set_sampler(uint32_t index, SamplerHandle sampler);
    /// Restricts later draws in this pass to a nonempty rectangle inside the pass attachments.
    /// Every pass starts with the scissor covering its attachments.
    RhiDiagnostic set_scissor(const ScissorRect& rect);
    /// Maps later draws in this pass into a nonempty rectangle inside the pass attachments (#1034).
    /// Every pass starts with the viewport covering its attachments.
    RhiDiagnostic set_viewport(const Viewport& viewport);
    /// Offsets the depth later draws in this pass write and test (#1034): by `constant` units of the depth
    /// format's resolution plus `slope` times the polygon's depth slope, at most `clamp` in magnitude
    /// (0: no clamp). Every pass starts with none. Shadow maps use it against self-shadowing.
    RhiDiagnostic set_depth_bias(float constant, float slope, float clamp = 0.0f);
    RhiDiagnostic draw(uint32_t vertex_count, uint32_t first_vertex = 0, uint32_t instance_count = 1);
    RhiDiagnostic draw_indexed(BufferHandle indices, IndexType type, uint32_t index_count,
                               size_t offset = 0, uint32_t instance_count = 1);
    /// Indices from this frame's upload memory; `offset` is relative to the slice and the indices
    /// must lie inside it.
    RhiDiagnostic draw_indexed(const TransientSlice& indices, IndexType type, uint32_t index_count,
                               size_t offset = 0, uint32_t instance_count = 1);
    RhiDiagnostic end_render_pass();
    /// Presents an acquired surface and submits. An open pass is closed and reported as wrong_state.
    RhiDiagnostic end_frame();
    /// Blocks until submitted frames complete, then releases every retired resource.
    void wait_idle() noexcept;
    /// GPU execution failures reported by completed frames since the last call.
    std::vector<RhiDiagnostic> take_gpu_errors();
    /// Whether completed frames report their GPU execution time. When false, GPU time is unavailable;
    /// it is never estimated from CPU submission time.
    bool gpu_timing_supported() const noexcept { return m_session != 0 && backend_gpu_timing_supported(); }
    /// GPU execution times of frames completed since the last call, in completion order, each with its
    /// passes when pass timing is on. At most RhiCompletion::timing_capacity are kept; `dropped` (if
    /// given) receives how many were lost.
    std::vector<GpuFrameTiming> take_gpu_timings(uint64_t* dropped = nullptr);

    /// Passes timed per frame; later passes in a frame are counted as untimed.
    static constexpr uint32_t max_timed_passes = 64;
    /// Whether frames report each pass's GPU time. Never estimated: when false,
    /// gpu_pass_timing_unavailable() says why.
    bool gpu_pass_timing_supported() const noexcept { return m_session != 0 && m_pass_timing && backend_pass_timing_reason().empty(); }
    std::string gpu_pass_timing_unavailable() const;
    /// Turns pass timing on (the default) or off for frames begun afterwards, e.g. for matched runs.
    void set_gpu_pass_timing(bool enabled) noexcept { m_pass_timing = enabled; }
    bool gpu_pass_timing_enabled() const noexcept { return m_pass_timing; }

    /// Whether presented frames report when they were shown (take_present_timings).
    bool present_timing_supported() const noexcept { return m_session != 0 && backend_present_timing_supported(); }
    /// Presented frames whose display time is known since the last call, in the order they were shown.
    std::vector<PresentTiming> take_present_timings(uint64_t* dropped = nullptr);
    /// The refresh rate of the display the surface is on, in Hz, or nullopt (headless, or unknown).
    std::optional<double> display_refresh_rate() const noexcept { return m_session ? backend_display_refresh_rate() : std::nullopt; }
    /// Memory the platform reports as allocated for this device (Metal: currentAllocatedSize), or
    /// nullopt when it cannot say. On unified memory it overlaps process memory; never add the two.
    std::optional<size_t> reported_memory() const noexcept { return m_session ? backend_reported_memory() : std::nullopt; }

    static std::unique_ptr<GraphicsDevice> create_default();

protected:
    enum class ResourceKind : uint8_t { buffer, texture, sampler, pipeline };
    struct BackendSurface {
        uint32_t width = 0;
        uint32_t height = 0;
        RhiDiagnostic diagnostic;
    };

    /// Set limits and surface format. Return false after releasing anything partially created.
    virtual bool backend_initialize(void* native_window, RhiLimits& limits, Format& surface_format) = 0;
    /// Called with no frame open and every slot already released.
    virtual void backend_shutdown() noexcept = 0;
    virtual void backend_resize(uint32_t width, uint32_t height) = 0;
    virtual RhiDiagnostic backend_create_buffer(uint32_t slot, const BufferDesc& desc, const void* data) = 0;
    virtual RhiDiagnostic backend_create_texture(uint32_t slot, const TextureDesc& desc, const void* data) = 0;
    virtual RhiDiagnostic backend_create_sampler(uint32_t slot, const SamplerDesc& desc) = 0;
    virtual RhiDiagnostic backend_create_pipeline(uint32_t slot, const PipelineDesc& desc) = 0;
    virtual void backend_release(ResourceKind kind, uint32_t slot) noexcept = 0;
    virtual void backend_write_buffer(uint32_t slot, size_t offset, const void* data, size_t size) noexcept = 0;
    virtual RhiDiagnostic backend_read_texture(uint32_t slot, const TextureDesc& desc, std::vector<std::byte>& pixels) = 0;
    virtual RhiDiagnostic backend_begin_frame() = 0;
    /// Bind the presentable texture to `slot` for this frame, or return a diagnostic.
    virtual BackendSurface backend_acquire_surface(uint32_t slot) = 0;
    /// Handles in `desc` are validated; their slots index backend storage.
    virtual RhiDiagnostic backend_begin_pass(const RenderPassDesc& desc) = 0;
    virtual void backend_set_pipeline(uint32_t slot) = 0;
    virtual void backend_set_vertex_buffer(uint32_t index, uint32_t slot, size_t offset) = 0;
    virtual void backend_set_uniform_buffer(uint32_t index, uint32_t slot, size_t offset) = 0;
    virtual void backend_set_texture(uint32_t index, uint32_t slot) = 0;
    virtual void backend_set_sampler(uint32_t index, uint32_t slot) = 0;
    /// The rectangle is validated against the pass attachments.
    virtual void backend_set_scissor(const ScissorRect& rect) = 0;
    virtual void backend_set_viewport(const Viewport& viewport) = 0;
    virtual void backend_set_depth_bias(float constant, float slope, float clamp) = 0;
    virtual void backend_draw(uint32_t vertex_count, uint32_t first_vertex, uint32_t instance_count) = 0;
    virtual void backend_draw_indexed(uint32_t slot, IndexType type, uint32_t index_count, size_t offset,
                                      uint32_t instance_count) = 0;
    virtual void backend_end_pass() = 0;
    /// Commit the frame (presenting the acquired surface, if any). Completion must eventually call
    /// completion()->complete(serial), from any thread; failures call report().
    virtual void backend_submit(uint64_t serial, bool present) = 0;
    /// Discard an open frame during shutdown without presenting.
    virtual void backend_abandon_frame() noexcept = 0;
    /// Block until every submitted frame has finished executing.
    virtual void backend_wait_idle() noexcept = 0;
    /// Block until frame `serial` has finished (so have all earlier frames). Return false if the
    /// backend cannot wait for it, which begin_frame reports as a timeout.
    virtual bool backend_wait_frame(uint64_t serial) noexcept = 0;
    /// Release the transient surface texture bound to `slot` at the end of a frame.
    virtual void backend_release_surface(uint32_t slot) noexcept = 0;
    /// Backends that call completion()->record_timing for every completed frame return true.
    virtual bool backend_gpu_timing_supported() const noexcept { return false; }
    virtual std::optional<size_t> backend_reported_memory() const noexcept { return std::nullopt; }
    /// Empty when the backend can time passes; otherwise why it cannot.
    virtual std::string backend_pass_timing_reason() const { return "this device does not sample GPU timestamps"; }
    /// Fills `timing.passes` and `timing.untimed_passes` for a completed frame (called on the owner thread).
    virtual void backend_attach_pass_timings(GpuFrameTiming&) {}
    virtual bool backend_present_timing_supported() const noexcept { return false; }
    virtual std::optional<double> backend_display_refresh_rate() const noexcept { return std::nullopt; }
    /// The current frame's passes so far (its index for the pass being begun), and whether they are timed.
    uint32_t frame_pass_index() const noexcept { return m_counters.frame_passes; }
    /// The serial the frame being begun or encoded will be submitted as.
    uint64_t encoding_frame_serial() const noexcept { return m_submitted + 1; }
    bool frame_pass_timing() const noexcept { return m_frame_pass_timing; }

    const std::shared_ptr<RhiCompletion>& completion() const noexcept { return m_completion; }

private:
    template<class Desc> struct Slot {
        Desc desc{};
        uint32_t generation = 1;
        bool live = false;
        bool internal = false; // device-owned; not destroyable or directly bindable by callers
    };
    template<class Desc> struct Table {
        std::vector<Slot<Desc>> slots;
        std::vector<uint32_t> free;
        size_t live = 0;
    };
    struct Retirement {
        ResourceKind kind;
        uint32_t slot;
        uint64_t serial;
    };
    enum class State : uint8_t { idle, frame, pass };

    template<class Desc> uint32_t allocate(Table<Desc>& table);
    template<class Desc, class Tag>
    const Desc* lookup(const Table<Desc>& table, RhiHandle<Tag> handle, const char* kind,
                       RhiDiagnostic* diagnostic) const;
    template<class Desc, class Tag>
    bool retire(Table<Desc>& table, ResourceKind kind, RhiHandle<Tag> handle) noexcept;
    template<class Desc> void free_slot(Table<Desc>& table, uint32_t slot) noexcept;
    void collect_retired() noexcept;
    void release_slot(ResourceKind kind, uint32_t slot) noexcept;
    RhiDiagnostic require_state(State state, const char* operation) const;
    RhiDiagnostic bind_vertex(uint32_t index, BufferHandle buffer, size_t offset, bool allow_internal);
    RhiDiagnostic bind_uniform(uint32_t index, BufferHandle buffer, size_t offset, bool allow_internal);
    RhiDiagnostic check_slice(const TransientSlice& slice) const;
    RhiDiagnostic encode_indexed(BufferHandle indices, IndexType type, uint32_t index_count, size_t offset,
                                 uint32_t instance_count, const TransientSlice* slice);
    void count_draw(uint32_t elements, uint32_t instances) noexcept;
    RhiDiagnostic validate_attachment(const TextureHandle& handle, bool depth, uint32_t& width,
                                      uint32_t& height) const;

    uint64_t m_session = 0;
    State m_state = State::idle;
    DeviceOptions m_options{};
    RhiLimits m_limits{};
    Format m_surface_format = Format::undefined;
    Table<BufferDesc> m_buffers;
    Table<TextureDesc> m_textures;
    Table<SamplerDesc> m_samplers;
    Table<PipelineDesc> m_pipelines;
    std::vector<Retirement> m_retirements;
    uint64_t m_submitted = 0;
    std::vector<BufferHandle> m_transient; // one upload buffer per frame slot
    size_t m_transient_used = 0;
    RhiStats m_counters{}; // wait and upload counters; resource counts are computed in stats()
    std::shared_ptr<RhiCompletion> m_completion = std::make_shared<RhiCompletion>();
    std::shared_ptr<const GraphicsResourceLifetime> m_resource_lifetime;

    // Per-frame encoder state.
    std::optional<SurfaceTarget> m_surface;
    std::vector<Format> m_pass_colors;
    Format m_pass_depth = Format::undefined;
    std::vector<TextureHandle> m_pass_attachments;
    uint32_t m_pass_width = 0;
    uint32_t m_pass_height = 0;
    bool m_pipeline_set = false;
    bool m_pass_timing = true; // as set; applies from the next begin_frame
    bool m_frame_pass_timing = false; // this frame's passes are timed
};

} // namespace maya
