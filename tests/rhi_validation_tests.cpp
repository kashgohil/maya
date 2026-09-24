#include "maya/rhi/null_device.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>

using namespace maya;

namespace {
constexpr auto color_format = Format::rgba8_unorm;

TextureHandle target(GraphicsDevice& device, uint32_t size = 8, Format format = color_format,
                     TextureUsage usage = TextureUsage::render_target) {
    auto created = device.create_texture({size, size, format, usage, "target"});
    REQUIRE(created);
    return created.handle;
}
PipelineDesc pipeline_desc(std::vector<Format> colors = {color_format}, Format depth = Format::undefined) {
    auto desc = PipelineDesc{};
    desc.shader_source = "source";
    desc.color_formats = std::move(colors);
    desc.depth_format = depth;
    desc.label = "test";
    return desc;
}
PipelineHandle pipeline(GraphicsDevice& device, std::vector<Format> colors = {color_format},
                        Format depth = Format::undefined) {
    auto created = device.create_pipeline(pipeline_desc(std::move(colors), depth));
    REQUIRE(created);
    return created.handle;
}
BufferHandle buffer(GraphicsDevice& device, BufferUsage usage, size_t size = 1024) {
    auto created = device.create_buffer({size, usage, "buffer"});
    REQUIRE(created);
    return created.handle;
}
RenderPassDesc color_pass(TextureHandle color) {
    auto pass = RenderPassDesc{};
    pass.colors.push_back({color});
    return pass;
}
/// Opens a frame and a single-attachment pass with a matching pipeline.
void open(GraphicsDevice& device, TextureHandle color, PipelineHandle pipe) {
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass(color_pass(color)));
    REQUIRE_FALSE(device.set_pipeline(pipe));
}
RhiError code(const RhiDiagnostic& diagnostic) { return diagnostic.code; }
template<class H> RhiError code(const RhiResult<H>& result) { return result.diagnostic.code; }
RhiError code(const SurfaceResult& result) { return result.diagnostic.code; }
} // namespace

TEST_CASE("Descriptors are validated before reaching the backend", "[rhi-api]") {
    NullGraphicsDevice device;
    CHECK(code(device.create_buffer({64, BufferUsage::vertex, "invalid"})) == RhiError::device_unavailable);
    REQUIRE(device.initialize(nullptr));
    const auto baseline = device.native_resources(); // the device's own per-frame upload buffers
    const auto limit = device.limits();
    CHECK(code(device.create_buffer({0, BufferUsage::vertex, "invalid"})) == RhiError::invalid_descriptor);
    CHECK(code(device.create_buffer({limit.max_buffer_size + 1, BufferUsage::vertex, "invalid"})) == RhiError::invalid_descriptor);
    CHECK(code(device.create_buffer({64, BufferUsage::none, "invalid"})) == RhiError::invalid_descriptor);
    CHECK(code(device.create_buffer({64, static_cast<BufferUsage>(8), "invalid"})) == RhiError::invalid_descriptor);
    CHECK(code(device.create_texture({0, 4, color_format, TextureUsage::sampled, "invalid"})) == RhiError::invalid_descriptor);
    CHECK(code(device.create_texture({limit.max_texture_dimension + 1, 4, color_format, TextureUsage::sampled, "invalid"})) ==
          RhiError::invalid_descriptor);
    CHECK(code(device.create_texture({4, 4, Format::undefined, TextureUsage::sampled, "invalid"})) == RhiError::invalid_descriptor);
    CHECK(code(device.create_texture({4, 4, color_format, TextureUsage::none, "invalid"})) == RhiError::invalid_descriptor);
    const float depth[16]{};
    CHECK(code(device.create_texture({4, 4, Format::depth32_float, TextureUsage::render_target, "invalid"}, depth)) == RhiError::unsupported);
    CHECK(code(device.create_texture({4, 4, Format::depth32_float, TextureUsage::readback, "invalid"})) == RhiError::unsupported);
    CHECK(code(device.create_sampler({static_cast<Filter>(9), Filter::linear, AddressMode::repeat, AddressMode::repeat, "invalid"})) == RhiError::invalid_descriptor);

    auto pipe = pipeline_desc();
    CHECK(device.create_pipeline(pipe));
    auto broken = pipe;
    broken.shader_source.clear();
    CHECK(code(device.create_pipeline(broken)) == RhiError::invalid_descriptor);
    broken = pipe;
    broken.color_formats.clear();
    CHECK(code(device.create_pipeline(broken)) == RhiError::invalid_descriptor);
    broken.color_formats = {Format::depth32_float};
    CHECK(code(device.create_pipeline(broken)) == RhiError::invalid_descriptor);
    broken = pipe;
    broken.color_formats.assign(limit.max_color_attachments + 1, color_format);
    CHECK(code(device.create_pipeline(broken)) == RhiError::invalid_descriptor);
    broken = pipe;
    broken.depth.test = true;
    const auto missing_depth = device.create_pipeline(broken);
    CHECK(code(missing_depth) == RhiError::invalid_descriptor);
    CHECK(missing_depth.diagnostic.message.find("depth") != std::string::npos);
    broken.depth_format = color_format;
    CHECK(code(device.create_pipeline(broken)) == RhiError::invalid_descriptor);
    CHECK(device.native_resources() == baseline + 1); // only the one valid pipeline reached the backend
}

TEST_CASE("Handles reject null, destroyed, reused, and foreign-session resources", "[rhi-api]") {
    NullGraphicsDevice device, other;
    REQUIRE(device.initialize(nullptr));
    REQUIRE(other.initialize(nullptr));
    const auto first = buffer(device, BufferUsage::uniform);
    const auto foreign = buffer(other, BufferUsage::uniform);
    const std::byte data[4]{};
    CHECK_FALSE(device.write_buffer(first, 0, data, 4));
    CHECK(code(device.write_buffer({}, 0, data, 4)) == RhiError::stale_handle);
    CHECK(device.write_buffer(foreign, 0, data, 4).message.find("another device session") != std::string::npos);
    CHECK(device.destroy(first));
    CHECK_FALSE(device.destroy(first));
    CHECK(code(device.write_buffer(first, 0, data, 4)) == RhiError::stale_handle);
    CHECK(device.describe(first) == nullptr);

    // The slot is reused with a new generation; the old handle stays invalid.
    const auto second = buffer(device, BufferUsage::uniform);
    CHECK(second.slot == first.slot);
    CHECK(second.generation != first.generation);
    CHECK(code(device.write_buffer(first, 0, data, 4)) == RhiError::stale_handle);
    CHECK_FALSE(device.write_buffer(second, 0, data, 4));

    // A new session invalidates every handle, even at the same slot and generation.
    REQUIRE(device.initialize(nullptr));
    const auto third = buffer(device, BufferUsage::uniform);
    CHECK(code(device.write_buffer(second, 0, data, 4)) == RhiError::stale_handle);
    CHECK(third.session != second.session);
    CHECK(code(device.write_buffer(third, 2, data, 4)) == RhiError::none);
    CHECK(code(device.write_buffer(third, 1021, data, 4)) == RhiError::out_of_range);
    CHECK(code(device.write_buffer(third, 0, nullptr, 4)) == RhiError::invalid_usage);
}

TEST_CASE("Encoder calls require the matching frame and pass state", "[rhi-api]") {
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    const auto color = target(device);
    const auto pipe = pipeline(device);
    CHECK(code(device.begin_render_pass(color_pass(color))) == RhiError::wrong_state);
    CHECK(code(device.end_frame()) == RhiError::wrong_state);
    CHECK(code(device.draw(3)) == RhiError::wrong_state);
    REQUIRE_FALSE(device.begin_frame());
    CHECK(code(device.begin_frame()) == RhiError::wrong_state);
    CHECK(code(device.set_pipeline(pipe)) == RhiError::wrong_state);
    REQUIRE_FALSE(device.begin_render_pass(color_pass(color)));
    CHECK(code(device.acquire_surface()) == RhiError::wrong_state);
    CHECK(code(device.begin_render_pass(color_pass(color))) == RhiError::wrong_state);
    CHECK(code(device.draw(3)) == RhiError::wrong_state); // no pipeline yet
    REQUIRE_FALSE(device.set_pipeline(pipe));
    CHECK_FALSE(device.draw(3));
    CHECK(code(device.draw(0)) == RhiError::invalid_usage);
    auto pixels = std::vector<std::byte>{};
    CHECK(code(device.read_texture(color, pixels)) == RhiError::wrong_state);

    // Ending a frame with an open pass closes it, reports the mistake, and still submits.
    const auto result = device.end_frame();
    CHECK(code(result) == RhiError::wrong_state);
    CHECK(result.message.find("still open") != std::string::npos);
    CHECK(device.stats().submitted_frames == 1);

    // Pipelines do not carry over between passes.
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass(color_pass(color)));
    CHECK(code(device.draw(3)) == RhiError::wrong_state);
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
}

TEST_CASE("Render passes validate attachments and load/store descriptions", "[rhi-api]") {
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    const auto color = target(device, 8);
    const auto small = target(device, 4);
    const auto depth = target(device, 8, Format::depth32_float);
    const auto sampled = target(device, 8, color_format, TextureUsage::sampled);
    REQUIRE_FALSE(device.begin_frame());
    const auto fails = [&](RenderPassDesc pass, RhiError expected, std::string_view excerpt) {
        const auto result = device.begin_render_pass(pass);
        INFO(result.message);
        CHECK(result.code == expected);
        CHECK(result.message.find(excerpt) != std::string::npos);
    };
    fails({}, RhiError::invalid_descriptor, "no attachments");
    auto pass = color_pass(color);
    pass.colors.push_back({small});
    fails(pass, RhiError::invalid_descriptor, "must be 8x8");
    fails(color_pass(sampled), RhiError::invalid_usage, "render_target usage");
    fails(color_pass(depth), RhiError::invalid_usage, "cannot be a color attachment");
    pass = color_pass(color);
    pass.depth = DepthAttachment{color};
    fails(pass, RhiError::invalid_usage, "cannot be a depth attachment");
    pass.depth = DepthAttachment{depth, LoadAction::clear, StoreAction::dont_care, 1.5};
    fails(pass, RhiError::invalid_descriptor, "clear depth");
    pass = color_pass(color);
    pass.colors.push_back({color});
    fails(pass, RhiError::invalid_usage, "same texture");
    pass = color_pass(color);
    pass.colors[0].clear_color[1] = std::numeric_limits<double>::infinity();
    fails(pass, RhiError::invalid_descriptor, "nonfinite");
    pass = color_pass(color);
    pass.colors[0].load = static_cast<LoadAction>(7);
    fails(pass, RhiError::invalid_descriptor, "load or store");
    pass = color_pass(color);
    pass.colors.assign(device.limits().max_color_attachments + 1, {color});
    fails(pass, RhiError::invalid_descriptor, "color attachments");

    // A depth-only pass and a color+depth pass are both valid.
    auto depth_only = RenderPassDesc{};
    depth_only.depth = DepthAttachment{depth, LoadAction::clear, StoreAction::store, 1.0};
    REQUIRE_FALSE(device.begin_render_pass(depth_only));
    REQUIRE_FALSE(device.end_render_pass());
    pass = color_pass(color);
    pass.depth = DepthAttachment{depth, LoadAction::load, StoreAction::store};
    REQUIRE_FALSE(device.begin_render_pass(pass));
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
}

TEST_CASE("Pipelines must match the pass attachment formats", "[rhi-api]") {
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    const auto color = target(device);
    const auto depth = target(device, 8, Format::depth32_float);
    const auto matching = pipeline(device, {color_format}, Format::depth32_float);
    auto pass = color_pass(color);
    pass.depth = DepthAttachment{depth};
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass(pass));
    for (const auto& [colors, depth_format] : std::vector<std::pair<std::vector<Format>, Format>>{
             {{Format::bgra8_unorm}, Format::depth32_float},
             {{color_format}, Format::undefined},
             {{color_format, color_format}, Format::depth32_float}}) {
        const auto result = device.set_pipeline(pipeline(device, colors, depth_format));
        INFO(result.message);
        CHECK(result.code == RhiError::incompatible_pipeline);
        CHECK(result.message.find("but the pass has [rgba8_unorm] + depth depth32_float") != std::string::npos);
    }
    CHECK_FALSE(device.set_pipeline(matching));
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
}

TEST_CASE("Bindings validate usage, offsets, ranges, slots, and attachment hazards", "[rhi-api]") {
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    const auto color = target(device, 8, color_format, TextureUsage::render_target | TextureUsage::sampled);
    const auto texture = target(device, 8, color_format, TextureUsage::sampled);
    const auto vertices = buffer(device, BufferUsage::vertex);
    const auto uniforms = buffer(device, BufferUsage::uniform);
    const auto indices = buffer(device, BufferUsage::index, 64);
    const auto sampler = device.create_sampler({});
    REQUIRE(sampler);
    const auto& limits = device.limits();
    open(device, color, pipeline(device));

    CHECK_FALSE(device.set_vertex_buffer(0, vertices, 16));
    CHECK(code(device.set_vertex_buffer(0, uniforms)) == RhiError::invalid_usage);
    CHECK(code(device.set_vertex_buffer(0, vertices, 2)) == RhiError::misaligned);
    CHECK(code(device.set_vertex_buffer(0, vertices, 1024)) == RhiError::out_of_range);
    CHECK(code(device.set_vertex_buffer(limits.max_buffer_slots, vertices)) == RhiError::out_of_range);

    CHECK_FALSE(device.set_uniform_buffer(1, uniforms, limits.uniform_offset_alignment));
    CHECK(code(device.set_uniform_buffer(1, vertices)) == RhiError::invalid_usage);
    CHECK(code(device.set_uniform_buffer(1, uniforms, 16)) == RhiError::misaligned);
    CHECK(code(device.set_uniform_buffer(1, uniforms, 1024)) == RhiError::out_of_range);

    CHECK_FALSE(device.set_texture(0, texture));
    CHECK(code(device.set_texture(0, color)) == RhiError::invalid_usage);
    CHECK(device.set_texture(0, color).message.find("attachment of the current pass") != std::string::npos);
    CHECK(code(device.set_texture(limits.max_texture_slots, texture)) == RhiError::out_of_range);
    CHECK_FALSE(device.set_sampler(0, sampler.handle));
    CHECK(code(device.set_sampler(limits.max_sampler_slots, sampler.handle)) == RhiError::out_of_range);

    CHECK_FALSE(device.draw_indexed(indices, IndexType::uint32, 16));
    CHECK_FALSE(device.draw_indexed(indices, IndexType::uint16, 30, 4));
    CHECK(code(device.draw_indexed(indices, IndexType::uint32, 17)) == RhiError::out_of_range);
    CHECK(code(device.draw_indexed(indices, IndexType::uint32, 15, 8)) == RhiError::out_of_range);
    CHECK(code(device.draw_indexed(indices, IndexType::uint32, 1, 2)) == RhiError::misaligned);
    CHECK(code(device.draw_indexed(indices, IndexType::uint32, 0)) == RhiError::invalid_usage);
    CHECK(code(device.draw_indexed(vertices, IndexType::uint32, 3)) == RhiError::invalid_usage);
    CHECK(code(device.draw_indexed(indices, IndexType::uint32, std::numeric_limits<uint32_t>::max())) ==
          RhiError::out_of_range);
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());

    std::vector<std::byte> pixels;
    CHECK(code(device.read_texture(color, pixels)) == RhiError::invalid_usage);
    const auto readable = target(device, 2, color_format, TextureUsage::readback);
    CHECK_FALSE(device.read_texture(readable, pixels));
    CHECK(pixels.size() == 16);
}

TEST_CASE("Destroyed resources are released only after their frames complete", "[rhi-api]") {
    NullGraphicsDevice device({false, 0, 0, true});
    REQUIRE(device.initialize(nullptr));
    const auto color = target(device);
    const auto pipe = pipeline(device);
    const auto baseline = device.native_resources();

    // Destroyed during encoding: retired after that frame completes.
    auto used = buffer(device, BufferUsage::vertex);
    open(device, color, pipe);
    REQUIRE_FALSE(device.set_vertex_buffer(0, used));
    CHECK(device.destroy(used));
    CHECK(code(device.set_vertex_buffer(0, used)) == RhiError::stale_handle);
    CHECK(device.stats().buffers == 0);
    CHECK(device.stats().pending_retirements == 1);
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    CHECK(device.native_resources() == baseline + 1); // submitted, not complete
    REQUIRE_FALSE(device.begin_frame());
    CHECK(device.native_resources() == baseline + 1);
    REQUIRE_FALSE(device.end_frame());
    device.complete_through(1);
    CHECK(device.stats().completed_frames == 1);
    REQUIRE_FALSE(device.begin_frame()); // completed retirements are collected at frame boundaries
    CHECK(device.native_resources() == baseline);
    CHECK(device.stats().pending_retirements == 0);
    REQUIRE_FALSE(device.end_frame());

    // Destroyed between frames while earlier frames are still in flight.
    auto late = buffer(device, BufferUsage::uniform);
    CHECK(device.destroy(late));
    CHECK(device.stats().pending_retirements == 1); // frames 2 and 3 are still in flight
    device.complete_through(2);
    REQUIRE_FALSE(device.begin_frame());
    CHECK(device.stats().pending_retirements == 1);
    REQUIRE_FALSE(device.end_frame());
    device.complete_through(3);
    device.wait_idle();
    CHECK(device.stats().pending_retirements == 0);
    CHECK(device.native_resources() == baseline);

    // Idle devices with no outstanding frames release immediately; slots are reused afterward.
    const auto reused = buffer(device, BufferUsage::uniform);
    CHECK(reused.slot == late.slot);
    CHECK(device.destroy(reused));
    CHECK(device.stats().pending_retirements == 0);

    // Retirement stays bounded across many create/use/destroy frames.
    for (uint64_t frame = 0; frame < 200; ++frame) {
        const auto transient = buffer(device, BufferUsage::vertex);
        open(device, color, pipe);
        REQUIRE_FALSE(device.set_vertex_buffer(0, transient));
        CHECK(device.destroy(transient));
        REQUIRE_FALSE(device.end_render_pass());
        REQUIRE_FALSE(device.end_frame());
        device.complete_through(device.stats().submitted_frames);
    }
    device.wait_idle();
    CHECK(device.stats().pending_retirements == 0);
    CHECK(device.native_resources() == baseline);
}

TEST_CASE("Completion watermarks tolerate out-of-order callbacks", "[rhi-api]") {
    RhiCompletion completion;
    completion.complete(2);
    completion.complete(3);
    CHECK(completion.completed.load() == 0);
    completion.complete(1);
    CHECK(completion.completed.load() == 3);
    completion.complete(3);
    completion.complete(5);
    CHECK(completion.completed.load() == 3);
    completion.complete_through(6);
    CHECK(completion.completed.load() == 6);
    CHECK(completion.early.empty());
    completion.report("lost device");
    NullGraphicsDevice device;
    REQUIRE(device.initialize(nullptr));
    CHECK(device.take_gpu_errors().empty());
}

TEST_CASE("Presentation is separate from offscreen rendering", "[rhi-api]") {
    int window = 0; // any non-null handle enables the emulated surface
    NullGraphicsDevice device({true, 0, 0, false});
    REQUIRE(device.initialize(&window));
    CHECK(device.surface_format() == Format::bgra8_unorm);
    const auto offscreen = target(device);
    const auto pipe = pipeline(device);

    // Zero-sized surface: acquisition fails, offscreen rendering continues, nothing is presented.
    REQUIRE_FALSE(device.begin_frame());
    CHECK(code(device.acquire_surface()) == RhiError::surface_unavailable);
    REQUIRE_FALSE(device.begin_render_pass(color_pass(offscreen)));
    REQUIRE_FALSE(device.set_pipeline(pipe));
    REQUIRE_FALSE(device.draw(3));
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    CHECK(device.presented_frames() == 0);

    device.resize(0, 0); // ignored
    device.resize(640, 360);
    REQUIRE_FALSE(device.begin_frame());
    const auto surface = device.acquire_surface();
    REQUIRE(surface);
    CHECK(surface.target.width == 640);
    CHECK(surface.target.height == 360);
    CHECK(device.acquire_surface().target.texture == surface.target.texture);
    CHECK_FALSE(device.destroy(surface.target.texture));
    CHECK(device.describe(surface.target.texture)->usage == TextureUsage::render_target);
    CHECK(code(device.set_pipeline(pipe)) == RhiError::wrong_state);
    REQUIRE_FALSE(device.begin_render_pass(color_pass(surface.target.texture)));
    CHECK(code(device.set_pipeline(pipe)) == RhiError::incompatible_pipeline); // pipeline targets rgba8
    REQUIRE_FALSE(device.set_pipeline(pipeline(device, {Format::bgra8_unorm})));
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    CHECK(device.presented_frames() == 1);
    CHECK(device.describe(surface.target.texture) == nullptr); // valid for one frame only
    CHECK(device.stats().textures == 1); // the surface is not a tracked resource

    // Missing drawable: skipped presentation, the frame still submits.
    device.set_drawable_available(false);
    REQUIRE_FALSE(device.begin_frame());
    CHECK(device.acquire_surface().diagnostic.message.find("No drawable") != std::string::npos);
    REQUIRE_FALSE(device.end_frame());
    CHECK(device.presented_frames() == 1);
    CHECK(device.stats().submitted_frames == 3);

    // Resize takes effect on the next acquisition.
    device.set_drawable_available(true);
    device.resize(320, 200);
    REQUIRE_FALSE(device.begin_frame());
    CHECK(device.acquire_surface().target.width == 320);
    REQUIRE_FALSE(device.end_frame());

    NullGraphicsDevice headless;
    REQUIRE(headless.initialize(nullptr));
    REQUIRE_FALSE(headless.begin_frame());
    CHECK(code(headless.acquire_surface()) == RhiError::unsupported);
    REQUIRE_FALSE(headless.end_frame());
}

TEST_CASE("Shutdown abandons open frames and releases every resource", "[rhi-api]") {
    int window = 0;
    NullGraphicsDevice device({true, 64, 64, true});
    for (int session = 0; session < 3; ++session) {
        REQUIRE(device.initialize(&window));
        const auto lifetime = device.resource_lifetime();
        const auto color = target(device);
        const auto kept = buffer(device, BufferUsage::vertex);
        const auto destroyed = buffer(device, BufferUsage::vertex);
        REQUIRE(device.create_sampler({}));
        open(device, color, pipeline(device));
        REQUIRE(device.acquire_surface().diagnostic.code == RhiError::wrong_state);
        CHECK(device.destroy(destroyed));
        device.shutdown(); // pass and frame still open; frame never completed
        device.shutdown();
        CHECK_FALSE(device.initialized());
        CHECK(lifetime.expired());
        CHECK(device.native_resources() == 0);
        CHECK(device.stats().pending_retirements == 0);
        CHECK(device.describe(kept) == nullptr);
        CHECK(code(device.begin_frame()) == RhiError::device_unavailable);
    }
    REQUIRE(device.initialize(&window));
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE(device.acquire_surface());
    device.shutdown(); // with an acquired, unpresented surface
    CHECK(device.presented_frames() == 0);
}

namespace {
/// Manual-completion device whose frame waits "block" by completing the requested frame.
class BlockingDevice final : public NullGraphicsDevice {
public:
    BlockingDevice() : NullGraphicsDevice({false, 0, 0, true}) {}
    ~BlockingDevice() override { shutdown(); }
    std::vector<uint64_t> waited;
    bool fail_submit = false;
protected:
    bool backend_wait_frame(uint64_t serial) noexcept override {
        waited.push_back(serial);
        complete_through(serial);
        return true;
    }
    void backend_submit(uint64_t serial, bool present) override {
        if (fail_submit) throw std::runtime_error("injected submission failure");
        NullGraphicsDevice::backend_submit(serial, present);
    }
};
void empty_frame(GraphicsDevice& device) {
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.end_frame());
}
} // namespace

TEST_CASE("Frames in flight are bounded, and waits are reported", "[rhi-api]") {
    NullGraphicsDevice invalid;
    CHECK_FALSE(invalid.initialize(nullptr, {0, 1024}));
    CHECK_FALSE(invalid.initialize(nullptr, {9, 1024}));
    CHECK_FALSE(invalid.initialize(nullptr, {3, invalid.limits().max_buffer_size + 1}));
    CHECK_FALSE(invalid.initialized());

    // A backend that cannot wait reports a timeout instead of reusing a busy frame slot.
    NullGraphicsDevice manual({false, 0, 0, true});
    REQUIRE(manual.initialize(nullptr, {2, 1024}));
    empty_frame(manual);
    empty_frame(manual);
    const auto blocked = manual.begin_frame();
    CHECK(blocked.code == RhiError::timeout);
    CHECK(blocked.message.find("Frame 1 has not completed") != std::string::npos);
    manual.complete_through(1);
    empty_frame(manual);
    CHECK(manual.begin_frame().code == RhiError::timeout);
    manual.complete_through(3);
    empty_frame(manual);

    // A blocking backend waits for exactly the frame that last used the slot.
    BlockingDevice device;
    REQUIRE(device.initialize(nullptr, {3, 1024}));
    for (int frame = 0; frame < 10; ++frame) {
        empty_frame(device);
        const auto stats = device.stats();
        CHECK(stats.submitted_frames - stats.completed_frames <= 3);
    }
    CHECK(device.waited == std::vector<uint64_t>{1, 2, 3, 4, 5, 6, 7});
    CHECK(device.stats().frame_waits == 7);
    CHECK(device.options().frames_in_flight == 3);
}

TEST_CASE("Upload memory is per frame, aligned, bounded, and recycled after completion", "[rhi-api]") {
    NullGraphicsDevice device({false, 0, 0, true});
    REQUIRE(device.initialize(nullptr, {2, 1024}));
    const auto baseline = device.native_resources();
    CHECK(baseline == 2);
    CHECK(device.stats().buffers == 0);
    const std::byte data[1024]{};
    CHECK(device.upload_transient(data, 16).diagnostic.code == RhiError::wrong_state);

    const auto color = target(device);
    open(device, color, pipeline(device));
    const auto first = device.upload_transient(data, 16);
    REQUIRE(first);
    CHECK(first.slice.offset == 0);
    CHECK(first.slice.frame == 1);
    const auto second = device.upload_transient(data, 16);
    CHECK(second.slice.offset == 256);
    const auto packed = device.upload_transient(data, 8, 4);
    CHECK(packed.slice.offset == 272);
    CHECK(device.upload_transient(data, 4, 3).diagnostic.code == RhiError::misaligned);
    CHECK(device.upload_transient(data, 4, 512).diagnostic.code == RhiError::misaligned);
    CHECK(device.upload_transient(nullptr, 4).diagnostic.code == RhiError::invalid_usage);
    CHECK(device.upload_transient(data, 0).diagnostic.code == RhiError::invalid_usage);
    const auto overflow = device.upload_transient(data, 1024);
    CHECK(overflow.diagnostic.code == RhiError::out_of_memory);
    CHECK(overflow.diagnostic.message.find("transient_bytes_per_frame") != std::string::npos);
    CHECK(device.stats().transient_failures == 1);
    CHECK(device.upload_transient(data, 64)); // the frame remains usable after exhaustion
    CHECK(device.stats().transient_bytes_used == 576);

    // Slices bind as uniforms or vertices; the raw upload buffer is not directly usable.
    CHECK_FALSE(device.set_uniform_buffer(1, first.slice));
    CHECK_FALSE(device.set_vertex_buffer(0, packed.slice));
    CHECK(device.set_uniform_buffer(1, packed.slice).code == RhiError::misaligned);
    CHECK(device.set_uniform_buffer(1, first.slice.buffer, 0).code == RhiError::invalid_usage);
    CHECK(device.draw_indexed(first.slice.buffer, IndexType::uint32, 3).code == RhiError::invalid_usage);
    CHECK(device.write_buffer(first.slice.buffer, 0, data, 4).code == RhiError::invalid_usage);
    CHECK_FALSE(device.destroy(first.slice.buffer));
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());

    // The next frame uses the other slot; the old slice cannot be bound.
    REQUIRE_FALSE(device.begin_frame());
    const auto next = device.upload_transient(data, 16);
    CHECK(next.slice.buffer != first.slice.buffer);
    CHECK(next.slice.offset == 0);
    REQUIRE_FALSE(device.begin_render_pass(color_pass(color)));
    CHECK(device.set_uniform_buffer(1, first.slice).code == RhiError::stale_handle);
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());

    // Frame 3 reuses frame 1's slot only after frame 1 completes.
    CHECK(device.begin_frame().code == RhiError::timeout);
    device.complete_through(1);
    REQUIRE_FALSE(device.begin_frame());
    CHECK(device.upload_transient(data, 16).slice.buffer == first.slice.buffer);
    REQUIRE_FALSE(device.end_frame());

    // Memory stays bounded across many frames.
    for (int frame = 0; frame < 1000; ++frame) {
        device.complete_through(device.stats().submitted_frames);
        REQUIRE_FALSE(device.begin_frame());
        for (int draw = 0; draw < 3; ++draw) REQUIRE(device.upload_transient(data, 200));
        REQUIRE_FALSE(device.end_frame());
    }
    CHECK(device.native_resources() == baseline + 2); // two upload buffers plus the target and pipeline
    CHECK(device.stats().transient_high_water <= 1024);

    NullGraphicsDevice disabled;
    REQUIRE(disabled.initialize(nullptr, {3, 0}));
    REQUIRE_FALSE(disabled.begin_frame());
    CHECK(disabled.upload_transient(data, 16).diagnostic.code == RhiError::unsupported);
    REQUIRE_FALSE(disabled.end_frame());
}

TEST_CASE("A failed submission completes its frame and still retires resources", "[rhi-api]") {
    BlockingDevice device;
    REQUIRE(device.initialize(nullptr, {1, 1024}));
    const auto doomed = buffer(device, BufferUsage::vertex);
    REQUIRE_FALSE(device.begin_frame());
    CHECK(device.destroy(doomed));
    device.fail_submit = true;
    CHECK_THROWS_AS(device.end_frame(), std::runtime_error);
    device.fail_submit = false;
    CHECK(device.stats().completed_frames == device.stats().submitted_frames);
    empty_frame(device); // no wait on the failed frame: it never reached the GPU
    CHECK(device.waited.empty());
    CHECK(device.stats().pending_retirements == 0);
}
