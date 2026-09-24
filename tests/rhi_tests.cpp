#include <catch2/catch_test_macros.hpp>
#include "maya/rhi/metal/metal_device.hpp"
#include "maya/assets/registry.hpp"
#include "maya/core/file_system.hpp"
#include <array>
#include <cstring>
#include <string>

using namespace maya;

namespace {
// Draws an axis-aligned rectangle from x_min to x_max at a constant depth and color.
constexpr auto rectangle_shader = R"(
    #include <metal_stdlib>
    using namespace metal;
    struct Params { float4 color; float depth; float x_min; float x_max; float pad; };
    constant float2 corners[6] = {float2(0,0), float2(1,0), float2(0,1), float2(1,0), float2(1,1), float2(0,1)};
    vertex float4 vertexMain(uint id [[vertex_id]], constant Params& p [[buffer(1)]]) {
        const float2 c = corners[id % 6];
        return float4(mix(p.x_min, p.x_max, c.x), mix(-1.0, 1.0, c.y), p.depth, 1.0);
    }
    fragment float4 fragmentMain(constant Params& p [[buffer(1)]]) { return p.color; }
    struct Sampled { float4 position [[position]]; float2 uv; };
    vertex Sampled vertexFull(uint id [[vertex_id]]) {
        const float2 c = corners[id % 6];
        return {float4(c * 2.0 - 1.0, 0.0, 1.0), float2(c.x, 1.0 - c.y)};
    }
    fragment float4 fragmentSample(Sampled in [[stage_in]], texture2d<float> image [[texture(0)]],
                                   sampler filter [[sampler(0)]]) { return image.sample(filter, in.uv); }
)";
struct Params {
    std::array<float, 4> color;
    float depth, x_min, x_max, pad;
};
using Pixel = std::array<uint8_t, 4>;
constexpr Pixel red{255, 0, 0, 255}, green{0, 255, 0, 255}, blue{0, 0, 255, 255}, yellow{255, 255, 0, 255};

Pixel pixel(const std::vector<std::byte>& pixels, uint32_t width, uint32_t x, uint32_t y) {
    auto value = Pixel{};
    std::memcpy(value.data(), pixels.data() + (size_t{y} * width + x) * 4, 4);
    return value;
}
std::vector<std::byte> read(GraphicsDevice& device, TextureHandle texture) {
    auto pixels = std::vector<std::byte>{};
    REQUIRE_FALSE(device.read_texture(texture, pixels));
    return pixels;
}
TextureHandle color_target(GraphicsDevice& device, uint32_t size) {
    auto created = device.create_texture({size, size, Format::rgba8_unorm,
        TextureUsage::render_target | TextureUsage::readback, "color target"});
    REQUIRE(created);
    return created.handle;
}
PipelineHandle rectangle_pipeline(GraphicsDevice& device, Format depth = Format::depth32_float) {
    auto desc = PipelineDesc{rectangle_shader, "vertexMain", "fragmentMain", {Format::rgba8_unorm}, depth,
        {depth != Format::undefined, depth != Format::undefined, CompareFunction::less}, CullMode::none,
        Winding::counter_clockwise, "rectangle"};
    auto created = device.create_pipeline(desc);
    INFO(created.diagnostic.message);
    REQUIRE(created);
    return created.handle;
}
BufferHandle params_buffer(GraphicsDevice& device, std::initializer_list<Params> params) {
    const auto stride = device.limits().uniform_offset_alignment;
    auto created = device.create_buffer({stride * params.size(), BufferUsage::uniform, "params"});
    REQUIRE(created);
    size_t offset = 0;
    for (const auto& value : params) {
        REQUIRE_FALSE(device.write_buffer(created.handle, offset, &value, sizeof(value)));
        offset += stride;
    }
    return created.handle;
}
/// Draws `bands` vertical stripes, each with its own per-draw uniform slice and color.
void draw_bands(GraphicsDevice& device, PipelineHandle pipeline, int bands, auto color_of) {
    REQUIRE_FALSE(device.set_pipeline(pipeline));
    for (int band = 0; band < bands; ++band) {
        const auto left = -1.0f + 2.0f * float(band) / float(bands);
        const auto params = Params{color_of(band), 0.5f, left, left + 2.0f / float(bands), 0.0f};
        const auto uploaded = device.upload_transient(&params, sizeof(params));
        REQUIRE(uploaded);
        REQUIRE_FALSE(device.set_uniform_buffer(1, uploaded.slice));
        REQUIRE_FALSE(device.draw(6));
    }
}
Pixel to_pixel(const std::array<float, 4>& color) {
    auto value = Pixel{};
    for (size_t i = 0; i < 4; ++i) value[i] = static_cast<uint8_t>(color[i] * 255.0f + 0.5f);
    return value;
}
void draw_rectangle(GraphicsDevice& device, PipelineHandle pipeline, BufferHandle params, size_t offset) {
    REQUIRE_FALSE(device.set_pipeline(pipeline));
    REQUIRE_FALSE(device.set_uniform_buffer(1, params, offset));
    REQUIRE_FALSE(device.draw(6));
}
} // namespace

TEST_CASE("Metal renders offscreen color/depth targets with explicit load and store actions", "[rhi]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr));
    constexpr uint32_t size = 16;
    const auto color = color_target(device, size);
    const auto depth = device.create_texture({size, size, Format::depth32_float, TextureUsage::render_target, "depth"});
    REQUIRE(depth);
    const auto pipeline = rectangle_pipeline(device);
    const auto stride = device.limits().uniform_offset_alignment;
    const auto params = params_buffer(device, {{{1, 0, 0, 1}, 0.5f, -1.0f, 0.0f, 0.0f},
                                               {{0, 1, 0, 1}, 0.7f, -1.0f, 1.0f, 0.0f}});

    // Pass 1 clears and stores both attachments; pass 2 loads them and depth-tests a full-width draw.
    REQUIRE_FALSE(device.begin_frame());
    auto first = RenderPassDesc{{{color, LoadAction::clear, StoreAction::store, {0, 0, 1, 1}}},
        DepthAttachment{depth.handle, LoadAction::clear, StoreAction::store, 1.0}, "first"};
    REQUIRE_FALSE(device.begin_render_pass(first));
    draw_rectangle(device, pipeline, params, 0);
    REQUIRE_FALSE(device.end_render_pass());
    auto second = RenderPassDesc{{{color, LoadAction::load, StoreAction::store}},
        DepthAttachment{depth.handle, LoadAction::load, StoreAction::dont_care}, "second"};
    REQUIRE_FALSE(device.begin_render_pass(second));
    draw_rectangle(device, pipeline, params, stride);
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    auto pixels = read(device, color);
    for (uint32_t y : {0u, size / 2, size - 1}) {
        CHECK(pixel(pixels, size, 0, y) == red);            // nearer first draw survived the depth test
        CHECK(pixel(pixels, size, size / 2 - 1, y) == red);
        CHECK(pixel(pixels, size, size / 2, y) == green);   // cleared depth 1.0 let the second draw pass
        CHECK(pixel(pixels, size, size - 1, y) == green);
    }

    // A clear-only pass replaces the previous contents.
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass({{{color, LoadAction::clear, StoreAction::store, {1, 1, 0, 1}}}, {}, "clear"}));
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    pixels = read(device, color);
    CHECK(pixel(pixels, size, 3, 7) == yellow);
    CHECK(pixel(pixels, size, 12, 2) == yellow);
    CHECK(device.take_gpu_errors().empty());
}

TEST_CASE("Metal uploads and samples textures", "[rhi]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr));
    const Pixel image[] = {red, green, blue, yellow}; // 2x2, row-major from the top
    const auto texture = device.create_texture({2, 2, Format::rgba8_unorm, TextureUsage::sampled, "image"}, image);
    REQUIRE(texture);
    const auto sampler = device.create_sampler({Filter::nearest, Filter::nearest, AddressMode::clamp_to_edge,
                                                AddressMode::clamp_to_edge, "nearest"});
    REQUIRE(sampler);
    auto desc = PipelineDesc{rectangle_shader, "vertexFull", "fragmentSample", {Format::rgba8_unorm},
        Format::undefined, {}, CullMode::none, Winding::counter_clockwise, "sample"};
    const auto pipeline = device.create_pipeline(desc);
    REQUIRE(pipeline);
    const auto color = color_target(device, 4);
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass({{{color}}, {}, "sample"}));
    REQUIRE_FALSE(device.set_pipeline(pipeline.handle));
    REQUIRE_FALSE(device.set_texture(0, texture.handle));
    REQUIRE_FALSE(device.set_sampler(0, sampler.handle));
    REQUIRE_FALSE(device.draw(6));
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    const auto pixels = read(device, color);
    CHECK(pixel(pixels, 4, 0, 0) == red);
    CHECK(pixel(pixels, 4, 3, 0) == green);
    CHECK(pixel(pixels, 4, 0, 3) == blue);
    CHECK(pixel(pixels, 4, 3, 3) == yellow);
}

TEST_CASE("Metal reports pipeline, compatibility, and binding errors", "[rhi]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr));
    auto desc = PipelineDesc{"this is not a shader", "vertexMain", "fragmentMain", {Format::rgba8_unorm},
        Format::undefined, {}, CullMode::none, Winding::counter_clockwise, "broken"};
    const auto broken = device.create_pipeline(desc);
    CHECK(broken.diagnostic.code == RhiError::shader_compilation);
    CHECK(broken.diagnostic.message.find("Pipeline 'broken': shader compilation failed") != std::string::npos);
    desc.shader_source = rectangle_shader;
    desc.fragment_entry = "missingEntry";
    const auto missing = device.create_pipeline(desc);
    CHECK(missing.diagnostic.code == RhiError::invalid_descriptor);
    CHECK(missing.diagnostic.message.find("'missingEntry' was not found") != std::string::npos);
    desc.fragment_entry = "fragmentMain";
    desc.color_formats = {Format::bgra8_unorm};
    const auto bgra = device.create_pipeline(desc);
    REQUIRE(bgra);
    CHECK(device.stats().pipelines == 1);

    const auto color = color_target(device, 8);
    const auto params = params_buffer(device, {{{1, 1, 1, 1}, 0.5f, -1, 1, 0}});
    const auto vertices = device.create_buffer({64, BufferUsage::vertex, "vertices"});
    REQUIRE(vertices);
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass({{{color}}, {}, "errors"}));
    CHECK(device.draw(6).code == RhiError::wrong_state);
    CHECK(device.set_pipeline(bgra.handle).code == RhiError::incompatible_pipeline);
    const auto pipeline = rectangle_pipeline(device, Format::undefined);
    REQUIRE_FALSE(device.set_pipeline(pipeline));
    CHECK(device.set_uniform_buffer(1, params, 4).code == RhiError::misaligned);
    CHECK(device.set_uniform_buffer(1, vertices.handle).code == RhiError::invalid_usage);
    CHECK(device.set_texture(0, color).code == RhiError::invalid_usage);
    CHECK(device.draw_indexed(vertices.handle, IndexType::uint32, 3).code == RhiError::invalid_usage);
    REQUIRE_FALSE(device.set_uniform_buffer(1, params));
    REQUIRE_FALSE(device.draw(6)); // rejected calls did not corrupt encoder state
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    CHECK(pixel(read(device, color), 8, 4, 4) == Pixel{255, 255, 255, 255});
    CHECK(device.take_gpu_errors().empty());
}

TEST_CASE("Metal retires destroyed resources only after their frames complete", "[rhi]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr));
    const auto color = color_target(device, 8);
    const auto pipeline = rectangle_pipeline(device, Format::undefined);
    const auto baseline = device.native_buffer_count();

    // Frame command buffers do not retain resources: correct output proves deferred retirement.
    constexpr int frames = 60;
    for (int frame = 0; frame < frames; ++frame) {
        const auto shade = static_cast<float>(frame % 2);
        const auto params = params_buffer(device, {{{shade, 1 - shade, 0, 1}, 0.5f, -1, 1, 0}});
        REQUIRE_FALSE(device.begin_frame());
        REQUIRE_FALSE(device.begin_render_pass({{{color}}, {}, "transient"}));
        draw_rectangle(device, pipeline, params, 0);
        CHECK(device.destroy(params)); // while the frame is still encoding
        CHECK(device.stats().buffers == 0);
        CHECK(device.native_buffer_count() == baseline + device.stats().pending_retirements);
        REQUIRE_FALSE(device.end_render_pass());
        REQUIRE_FALSE(device.end_frame());
        CHECK(device.stats().pending_retirements <= static_cast<size_t>(frame + 1));
    }
    CHECK(pixel(read(device, color), 8, 4, 4) == red); // frame 59 draws shade 1 = red
    device.wait_idle();
    CHECK(device.stats().pending_retirements == 0);
    CHECK(device.native_buffer_count() == baseline);
    CHECK(device.stats().completed_frames == frames);

    // Destroying a texture and pipeline mid-frame is also safe.
    const auto other = color_target(device, 8);
    const auto doomed = rectangle_pipeline(device, Format::undefined);
    const auto params = params_buffer(device, {{{0, 0, 1, 1}, 0.5f, -1, 1, 0}});
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass({{{other}}, {}, "doomed"}));
    draw_rectangle(device, doomed, params, 0);
    CHECK(device.destroy(doomed));
    CHECK(device.destroy(params));
    REQUIRE_FALSE(device.end_render_pass());
    CHECK(device.destroy(other)); // still referenced by the submitted frame
    REQUIRE_FALSE(device.end_frame());
    device.wait_idle();
    CHECK(device.stats().pending_retirements == 0);
    CHECK(device.take_gpu_errors().empty());
}

TEST_CASE("Metal sessions handle headless surfaces, open-frame shutdown, and stale handles", "[rhi]") {
    MetalDevice device;
    for (int session = 0; session < 3; ++session) {
        REQUIRE(device.initialize(nullptr));
        CHECK(device.surface_format() == Format::undefined);
        device.resize(1920, 1080); // no surface: ignored
        const auto color = color_target(device, 8);
        const auto pipeline = rectangle_pipeline(device, Format::undefined);
        const auto params = params_buffer(device, {{{1, 0, 0, 1}, 0.5f, -1, 1, 0}});
        REQUIRE_FALSE(device.begin_frame());
        CHECK(device.acquire_surface().diagnostic.code == RhiError::unsupported);
        REQUIRE_FALSE(device.begin_render_pass({{{color}}, {}, "abandoned"}));
        draw_rectangle(device, pipeline, params, 0);
        device.shutdown(); // abandons the encoding frame
        CHECK(device.native_buffer_count() == 0);
        CHECK(device.native_texture_count() == 0);
        CHECK(device.describe(color) == nullptr);
        REQUIRE(device.initialize(nullptr));
        CHECK(device.write_buffer(params, 0, &session, sizeof(session)).code == RhiError::stale_handle);
        device.shutdown();
    }
}

TEST_CASE("Metal asset sharing and buffer retirement survive repeated sessions", "[rhi][assets]") {
    MetalDevice device;
    const auto source = FileSystem::resolve("samples/basic_scene/assets/pyramid.obj");
    REQUIRE(source);
    const auto reference = AssetRef<MeshAsset>{{0x6d617961,1}};
    for (int session=0; session<3; ++session) {
        REQUIRE(device.initialize(nullptr));
        const auto baseline=device.native_buffer_count(); // per-frame upload buffers
        AssetRegistry registry(source->parent_path(),std::make_unique<FileAssetProvider>(device));
        REQUIRE_FALSE(registry.register_asset(reference,"pyramid.obj"));
        auto first=registry.acquire(reference), second=registry.acquire(reference);
        REQUIRE(first); REQUIRE(second);
        CHECK(&first.lease.value() == &second.lease.value());
        CHECK(device.native_buffer_count() == baseline + 2);
        first={}; CHECK(registry.evict_unused() == 0);
        second={}; CHECK(registry.evict_unused() == 1);
        CHECK(device.native_buffer_count() == baseline); // no frame in flight: released immediately
        auto held=registry.acquire(reference); REQUIRE(held);
        device.shutdown();
        CHECK(device.native_buffer_count() == 0);
        CHECK_FALSE(held.lease.value().mesh().valid());
        CHECK_FALSE(registry.resolve(held.lease.handle()));
    }
}

TEST_CASE("Metal per-draw uniforms keep distinct values within a frame", "[rhi]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr));
    constexpr int bands = 16;
    constexpr uint32_t size = 64;
    const auto color = color_target(device, size);
    const auto pipeline = rectangle_pipeline(device, Format::undefined);
    const auto color_of = [](int band) { return std::array<float, 4>{band / 15.0f, 1.0f - band / 15.0f, (band % 3) / 2.0f, 1}; };
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass({{{color}}, {}, "bands"}));
    draw_bands(device, pipeline, bands, color_of);
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    CHECK(device.stats().transient_bytes_used == (bands - 1) * device.limits().uniform_offset_alignment + sizeof(Params));
    const auto pixels = read(device, color);
    for (int band = 0; band < bands; ++band) {
        INFO("band " << band);
        CHECK(pixel(pixels, size, band * (size / bands) + 1, size / 2) == to_pixel(color_of(band)));
    }
}

TEST_CASE("Metal frames encoded ahead of the GPU never see later CPU writes", "[rhi]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr, {3, size_t{1} << 20}));
    constexpr int targets = 12, bands = 8, rounds = 5;
    constexpr uint32_t size = 32;
    auto colors = std::vector<TextureHandle>{};
    for (int i = 0; i < targets; ++i) colors.push_back(color_target(device, size));
    const auto pipeline = rectangle_pipeline(device, Format::undefined);
    const auto color_of = [](int round, int target, int band) {
        return std::array<float, 4>{float((round * 7 + target * 3 + band) % 16) / 15.0f,
            float((target + band * 5) % 16) / 15.0f, float((round + band) % 4) / 3.0f, 1};
    };
    for (int round = 0; round < rounds; ++round) {
        // Every frame rewrites upload memory without waiting; each renders its own target.
        for (int target = 0; target < targets; ++target) {
            REQUIRE_FALSE(device.begin_frame());
            const auto stats = device.stats();
            CHECK(stats.submitted_frames - stats.completed_frames < 3);
            REQUIRE_FALSE(device.begin_render_pass({{{colors[target]}}, {}, "ahead"}));
            // Extra uploads stress the allocator between the draws that are checked.
            const std::byte filler[192]{};
            for (int i = 0; i < 200; ++i) REQUIRE(device.upload_transient(filler, sizeof(filler)));
            draw_bands(device, pipeline, bands, [&](int band) { return color_of(round, target, band); });
            REQUIRE_FALSE(device.end_render_pass());
            REQUIRE_FALSE(device.end_frame());
        }
        for (int target = 0; target < targets; ++target) {
            const auto pixels = read(device, colors[target]);
            for (int band = 0; band < bands; ++band) {
                INFO("round " << round << " target " << target << " band " << band);
                CHECK(pixel(pixels, size, band * (size / bands) + 1, size / 2) == to_pixel(color_of(round, target, band)));
            }
        }
    }
    INFO("waits " << device.stats().frame_waits << " (" << device.stats().frame_wait_microseconds << " us)");
    CHECK(device.stats().submitted_frames == targets * rounds);
    CHECK(device.take_gpu_errors().empty());
    // Shutdown with frames still in flight drains them before releasing memory.
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass({{{colors[0]}}, {}, "final"}));
    draw_bands(device, pipeline, bands, [&](int band) { return color_of(0, 0, band); });
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    device.shutdown();
    CHECK(device.native_buffer_count() == 0);
}

TEST_CASE("Metal upload exhaustion skips draws without corrupting the frame", "[rhi]") {
    MetalDevice device;
    REQUIRE(device.initialize(nullptr, {3, 1024})); // room for four 256-byte constant slices
    constexpr uint32_t size = 40;
    const auto color = color_target(device, size);
    const auto pipeline = rectangle_pipeline(device, Format::undefined);
    REQUIRE_FALSE(device.begin_frame());
    REQUIRE_FALSE(device.begin_render_pass({{{color, LoadAction::clear, StoreAction::store, {0, 0, 0, 1}}}, {}, "overflow"}));
    REQUIRE_FALSE(device.set_pipeline(pipeline));
    int drawn = 0;
    for (int band = 0; band < 5; ++band) {
        const auto left = -1.0f + 0.4f * float(band);
        const auto params = Params{{0, 1, 0, 1}, 0.5f, left, left + 0.4f, 0.0f};
        const auto uploaded = device.upload_transient(&params, sizeof(params));
        if (!uploaded) {
            CHECK(uploaded.diagnostic.code == RhiError::out_of_memory);
            continue; // defined behavior: this draw is skipped, the frame continues
        }
        REQUIRE_FALSE(device.set_uniform_buffer(1, uploaded.slice));
        REQUIRE_FALSE(device.draw(6));
        ++drawn;
    }
    REQUIRE_FALSE(device.end_render_pass());
    REQUIRE_FALSE(device.end_frame());
    CHECK(drawn == 4);
    CHECK(device.stats().transient_failures == 1);
    const auto pixels = read(device, color);
    for (int band = 0; band < 4; ++band) CHECK(pixel(pixels, size, band * 8 + 2, size / 2) == green);
    CHECK(pixel(pixels, size, 4 * 8 + 2, size / 2) == Pixel{0, 0, 0, 255});
    CHECK(device.take_gpu_errors().empty());
}
