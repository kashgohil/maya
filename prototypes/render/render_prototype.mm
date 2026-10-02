// Rendering prototype for #1030, on Metal directly (the RHI has no compare samplers, layered or array
// targets, or RG11B10 yet). Two parts:
//  - Tone mapping: an HDR view of an environment and an exposure chart, through clamping, the ACES
//    fit, AgX, and Khronos PBR Neutral, written as PNGs to compare, with each pass's GPU time.
//  - Scene targets and cascaded shadows: 10,000 boxes and a floor lit by the sun at 1920x1080, with
//    the scene target in RGBA8, RGBA16F, and RG11B10F, and shadow maps of 3 or 4 cascades at 1024 or
//    2048 texels, filtered with 1, 9, or 25 taps. GPU times come from command-buffer timestamps.
// Usage: maya_render_prototype <render-samples folder> <output folder>
#include <stb_image.h>
#include <stb_image_write.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char* shader_source = R"(
#include <metal_stdlib>
using namespace metal;

struct Fullscreen { float4 position [[position]]; float2 uv; };
vertex Fullscreen fullscreen(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    Fullscreen out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

// The top three quarters look into the environment; the bottom quarter is a chart of eight colors
// from -6 to +10 stops around 18% grey.
fragment half4 hdr_scene(Fullscreen in [[stage_in]], texture2d<float> panorama [[texture(0)]], sampler s [[sampler(0)]],
                         constant float& exposure [[buffer(0)]]) {
    float3 c;
    if (in.uv.y < 0.75) {
        const float yaw = (in.uv.x - 0.5) * 2.6 + 0.6;
        const float pitch = (0.375 - in.uv.y) * 1.5 + 0.1;
        const float3 d = float3(sin(yaw) * cos(pitch), sin(pitch), -cos(yaw) * cos(pitch));
        const float2 uv = float2(0.5 + atan2(d.x, -d.z) / (2.0 * M_PI_F), acos(clamp(d.y, -1.0, 1.0)) / M_PI_F);
        c = panorama.sample(s, uv).rgb;
    } else {
        const float3 colors[8] = {float3(1, 1, 1), float3(1, 0.05, 0.05), float3(0.05, 1, 0.05), float3(0.05, 0.05, 1),
                                  float3(1, 1, 0.05), float3(0.05, 1, 1), float3(1, 0.05, 1), float3(0.8, 0.55, 0.42)};
        const int row = min(7, int((in.uv.y - 0.75) / 0.25 * 8.0));
        const float stops = floor(mix(-6.0, 10.0, in.uv.x) * 2.0) / 2.0; // half-stop steps
        c = colors[row] * 0.18 * exp2(stops);
    }
    return half4(half3(c * exposure), 1.0h);
}

float3 aces_fit(float3 v) { // Stephen Hill's fit of the ACES RRT and sRGB ODT
    const float3x3 input = float3x3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const float3x3 output = float3x3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    v = input * v;
    v = (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081);
    return output * v;
}
float3 agx(float3 v) { // AgX, default look (the common minimal approximation of Troy Sobotka's AgX)
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float min_ev = -12.47393, max_ev = 4.026069;
    v = inset * v;
    v = clamp(log2(max(v, 1e-10)), min_ev, max_ev);
    v = (v - min_ev) / (max_ev - min_ev);
    const float3 x2 = v * v, x4 = x2 * x2;
    v = 15.5 * x4 * x2 - 40.14 * x4 * v + 31.96 * x4 - 6.868 * x2 * v + 0.4298 * x2 + 0.1191 * v - 0.00232;
    v = outset * v;
    return pow(max(v, 0.0), 2.2); // back to linear; encoded again below
}
float3 pbr_neutral(float3 c) { // Khronos PBR Neutral
    const float start = 0.8 - 0.04, desaturation = 0.15;
    const float x = min(c.r, min(c.g, c.b));
    c -= x < 0.08 ? x - 6.25 * x * x : 0.04;
    const float peak = max(c.r, max(c.g, c.b));
    if (peak < start) return c;
    const float d = 1.0 - start;
    const float new_peak = 1.0 - d * d / (peak + d - start);
    c *= new_peak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(c, float3(new_peak), g);
}
float3 srgb_encode(float3 c) {
    c = saturate(c);
    return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, 12.92 * c, c <= 0.0031308);
}
fragment float4 tonemap(Fullscreen in [[stage_in]], texture2d<float> hdr [[texture(0)]], constant uint& op [[buffer(0)]]) {
    float3 c = hdr.read(uint2(in.position.xy)).rgb;
    if (op == 1) c = aces_fit(c);
    else if (op == 2) c = agx(c);
    else if (op == 3) c = pbr_neutral(c);
    return float4(srgb_encode(c), 1.0);
}

// Boxes and a floor, lit by the sun, with cascaded shadows from one depth atlas (2 x 2 cascades).
struct Vertex { packed_float3 position; packed_float3 normal; };
struct Frame {
    float4x4 view_projection;
    float4x4 view;
    float4x4 cascade[4];
    float4 splits; // view-space far distance of each cascade
    float4 sun; // xyz: toward the sun
    uint cascades, taps;
    float texel; // one atlas texel, in UV
    float pad;
};
vertex float4 shadow_vertex(uint vid [[vertex_id]], uint iid [[instance_id]], const device Vertex* vertices [[buffer(0)]],
                            const device float4x4* models [[buffer(1)]], constant float4x4& light [[buffer(2)]]) {
    return light * (models[iid] * float4(float3(vertices[vid].position), 1.0));
}
struct Lit { float4 position [[position]]; float3 world; float3 normal; float depth; };
vertex Lit lit_vertex(uint vid [[vertex_id]], uint iid [[instance_id]], const device Vertex* vertices [[buffer(0)]],
                      const device float4x4* models [[buffer(1)]], constant Frame& frame [[buffer(2)]]) {
    const float4 world = models[iid] * float4(float3(vertices[vid].position), 1.0);
    Lit out;
    out.position = frame.view_projection * world;
    out.world = world.xyz;
    out.normal = normalize((models[iid] * float4(float3(vertices[vid].normal), 0.0)).xyz);
    out.depth = -(frame.view * world).z;
    return out;
}
fragment half4 lit_fragment(Lit in [[stage_in]], depth2d<float> atlas [[texture(0)]], sampler compare [[sampler(0)]],
                            constant Frame& frame [[buffer(2)]]) {
    const float3 n = normalize(in.normal);
    const float ndl = saturate(dot(n, frame.sun.xyz));
    float shadow = 1.0;
    if (frame.taps > 0 && ndl > 0.0) {
        uint c = 0;
        while (c + 1 < frame.cascades && in.depth > frame.splits[c]) ++c;
        const float4 p = frame.cascade[c] * float4(in.world + n * 0.05, 1.0);
        const float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5) * 0.5 + float2(c % 2, c / 2) * 0.5;
        const int radius = frame.taps >= 25 ? 2 : frame.taps >= 9 ? 1 : 0;
        float lit = 0.0;
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x)
                lit += atlas.sample_compare(compare, uv + float2(x, y) * frame.texel, p.z - 0.0015);
        shadow = lit / float((2 * radius + 1) * (2 * radius + 1));
    }
    const float3 albedo = float3(0.6, 0.55, 0.5);
    return half4(half3(albedo * (0.15 + 3.0 * ndl * shadow)), 1.0h);
}
)";

struct Matrix { // column-major, as Metal's float4x4
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Matrix operator*(const Matrix& b) const {
        auto r = Matrix{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                auto s = 0.0f;
                for (int k = 0; k < 4; ++k) s += m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = s;
            }
        return r;
    }
};
using Vec3 = std::array<float, 3>;
Vec3 sub(Vec3 a, Vec3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float dot(Vec3 a, Vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 normalize(Vec3 a) {
    const auto l = std::sqrt(dot(a, a));
    return {a[0] / l, a[1] / l, a[2] / l};
}
Matrix look_at(Vec3 eye, Vec3 target, Vec3 up) {
    const auto f = normalize(sub(target, eye)), s = normalize(cross(f, up)), u = cross(s, f);
    auto r = Matrix{};
    r.m[0] = s[0]; r.m[4] = s[1]; r.m[8] = s[2];
    r.m[1] = u[0]; r.m[5] = u[1]; r.m[9] = u[2];
    r.m[2] = -f[0]; r.m[6] = -f[1]; r.m[10] = -f[2];
    r.m[12] = -dot(s, eye); r.m[13] = -dot(u, eye); r.m[14] = dot(f, eye);
    return r;
}
Matrix perspective(float fov, float aspect, float near, float far) { // depth 0 to 1
    auto r = Matrix{};
    const auto y = 1.0f / std::tan(fov / 2);
    r.m[0] = y / aspect; r.m[5] = y; r.m[10] = far / (near - far); r.m[11] = -1; r.m[14] = near * far / (near - far); r.m[15] = 0;
    return r;
}
Matrix orthographic(float half, float near, float far) {
    auto r = Matrix{};
    r.m[0] = 1 / half; r.m[5] = 1 / half; r.m[10] = 1 / (near - far); r.m[14] = near / (near - far);
    return r;
}

struct Frame {
    Matrix view_projection, view, cascade[4];
    float splits[4];
    float sun[4];
    uint32_t cascades, taps;
    float texel, pad;
};

double gpu_ms(id<MTLCommandBuffer> buffer) { return (buffer.GPUEndTime - buffer.GPUStartTime) * 1000.0; }

void write_png(id<MTLTexture> texture, const fs::path& file) {
    auto pixels = std::vector<uint8_t>(texture.width * texture.height * 4);
    [texture getBytes:pixels.data() bytesPerRow:texture.width * 4 fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height) mipmapLevel:0];
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    stbi_write_png(file.c_str(), int(texture.width), int(texture.height), 4, pixels.data(), int(texture.width) * 4);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: maya_render_prototype <render-samples folder> <output folder>\n");
        return 2;
    }
    if (!fs::exists(fs::path(argv[1]) / "Models")) {
        std::printf("No sample content in %s; run tools/fetch_render_samples.sh\n", argv[1]);
        return 77; // CTest's skip code for these prototypes
    }
    const auto samples = fs::path(argv[1]);
    const auto output = fs::path(argv[2]);
    fs::create_directories(output);

    // What the decision relies on fails the run (CTest's `prototype` label); the rest is reported.
    auto failures = 0;
    const auto unexpected = [&](const std::string& what) {
        std::printf("UNEXPECTED: %s\n", what.c_str());
        ++failures;
    };
    @autoreleasepool {
        auto device = MTLCreateSystemDefaultDevice();
        auto queue = [device newCommandQueue];
        NSError* error = nil;
        auto library = [device newLibraryWithSource:[NSString stringWithUTF8String:shader_source] options:nil error:&error];
        if (!library) {
            std::fprintf(stderr, "shader: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        const auto function = [&](const char* name) { return [library newFunctionWithName:[NSString stringWithUTF8String:name]]; };
        const auto pipeline = [&](const char* vertex, const char* fragment, MTLPixelFormat color, MTLPixelFormat depth) {
            auto desc = [MTLRenderPipelineDescriptor new];
            desc.vertexFunction = function(vertex);
            desc.fragmentFunction = fragment ? function(fragment) : nil;
            desc.colorAttachments[0].pixelFormat = color;
            desc.depthAttachmentPixelFormat = depth;
            NSError* e = nil;
            auto state = [device newRenderPipelineStateWithDescriptor:desc error:&e];
            if (!state) unexpected(std::string("pipeline ") + vertex + ": " + e.localizedDescription.UTF8String);
            return state;
        };
        const auto target = [&](MTLPixelFormat format, NSUInteger w, NSUInteger h, bool shared) {
            auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
            desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            desc.storageMode = shared ? MTLStorageModeShared : MTLStorageModePrivate;
            return [device newTextureWithDescriptor:desc];
        };
        constexpr NSUInteger width = 1920, height = 1080;

        // Part 1: tone mapping.
        int pw = 0, ph = 0, channels = 0;
        auto* hdr = stbi_loadf((samples / "hdri/aerodynamics_workshop_2k.hdr").c_str(), &pw, &ph, &channels, 4);
        if (!hdr) {
            unexpected("the HDR environment cannot be loaded; run tools/fetch_render_samples.sh");
            return 1;
        }
        if (failures > 0) return 1;
        auto halves = std::vector<__fp16>(size_t(pw) * ph * 4);
        for (size_t i = 0; i < halves.size(); ++i) halves[i] = __fp16(hdr[i]);
        stbi_image_free(hdr);
        auto panorama_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:pw height:ph mipmapped:NO];
        auto panorama = [device newTextureWithDescriptor:panorama_desc];
        [panorama replaceRegion:MTLRegionMake2D(0, 0, pw, ph) mipmapLevel:0 withBytes:halves.data() bytesPerRow:size_t(pw) * 8];
        auto sampler_desc = [MTLSamplerDescriptor new];
        sampler_desc.minFilter = sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
        sampler_desc.sAddressMode = MTLSamplerAddressModeRepeat;
        auto linear = [device newSamplerStateWithDescriptor:sampler_desc];
        auto scene_target = target(MTLPixelFormatRGBA16Float, width, height, false);
        auto ldr = target(MTLPixelFormatRGBA8Unorm, width, height, true);
        auto scene_pipeline = pipeline("fullscreen", "hdr_scene", MTLPixelFormatRGBA16Float, MTLPixelFormatInvalid);
        auto tonemap_pipeline = pipeline("fullscreen", "tonemap", MTLPixelFormatRGBA8Unorm, MTLPixelFormatInvalid);
        const auto pass = [&](id<MTLCommandBuffer> buffer, id<MTLTexture> color, id<MTLRenderPipelineState> state, auto&& bind) {
            auto rp = [MTLRenderPassDescriptor new];
            rp.colorAttachments[0].texture = color;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            auto encoder = [buffer renderCommandEncoderWithDescriptor:rp];
            [encoder setRenderPipelineState:state];
            bind(encoder);
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
        };
        const auto exposure = 0.6f;
        {
            auto buffer = [queue commandBuffer];
            pass(buffer, scene_target, scene_pipeline, [&](id<MTLRenderCommandEncoder> e) {
                [e setFragmentTexture:panorama atIndex:0];
                [e setFragmentSamplerState:linear atIndex:0];
                [e setFragmentBytes:&exposure length:sizeof(exposure) atIndex:0];
            });
            [buffer commit];
            [buffer waitUntilCompleted];
        }
        std::printf("Tone mapping at %lux%lu (GPU ms per pass, median of 50)\n", (unsigned long)width, (unsigned long)height);
        const char* names[] = {"clamp", "aces-fit", "agx", "pbr-neutral"};
        for (uint32_t op = 0; op < 4; ++op) {
            auto times = std::vector<double>{};
            for (int i = 0; i < 50; ++i) {
                auto buffer = [queue commandBuffer];
                pass(buffer, ldr, tonemap_pipeline, [&](id<MTLRenderCommandEncoder> e) {
                    [e setFragmentTexture:scene_target atIndex:0];
                    [e setFragmentBytes:&op length:sizeof(op) atIndex:0];
                });
                [buffer commit];
                [buffer waitUntilCompleted];
                times.push_back(gpu_ms(buffer));
            }
            std::ranges::sort(times);
            const auto file = output / (std::string("tonemap-") + names[op] + ".png");
            write_png(ldr, file);
            std::printf("  %-12s %.3f ms   %s\n", names[op], times[25], file.c_str());
        }

        // Part 2: scene targets and cascaded shadows.
        auto vertices = std::vector<float>{}; // 36 vertices: position, normal
        const float faces[6][3][3] = {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                                      {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                                      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
        for (const auto& [n, u, v] : faces) {
            const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
            for (const auto& [a, b] : corners) {
                for (int k = 0; k < 3; ++k) vertices.push_back(n[k] * 0.5f + u[k] * 0.5f * a + v[k] * 0.5f * b);
                for (int k = 0; k < 3; ++k) vertices.push_back(n[k]);
            }
        }
        auto vertex_buffer = [device newBufferWithBytes:vertices.data() length:vertices.size() * 4 options:MTLResourceStorageModeShared];
        auto models = std::vector<Matrix>{};
        auto floor = Matrix{};
        floor.m[0] = 220; floor.m[5] = 1; floor.m[10] = 220; floor.m[13] = -0.5f;
        models.push_back(floor);
        for (int i = 0; i < 10000; ++i) { // a 100 x 100 grid, 2 m apart, of boxes 1 to 6 m tall
            auto box = Matrix{};
            const auto h = 1.0f + float((i * 7919) % 11) * 0.5f;
            box.m[0] = 1.2f; box.m[5] = h; box.m[10] = 1.2f;
            box.m[12] = float(i % 100) * 2.0f - 99.0f; box.m[13] = h / 2; box.m[14] = float(i / 100) * 2.0f - 99.0f;
            models.push_back(box);
        }
        auto model_buffer = [device newBufferWithBytes:models.data() length:models.size() * sizeof(Matrix) options:MTLResourceStorageModeShared];
        const auto eye = Vec3{-20, 18, 60}, at = Vec3{10, 0, -10};
        const auto near = 0.3f, far = 220.0f, fov = 1.0f, aspect = float(width) / float(height);
        const auto sun = normalize(Vec3{0.45f, 0.75f, 0.35f});
        auto frame = Frame{};
        frame.view = look_at(eye, at, {0, 1, 0});
        frame.view_projection = perspective(fov, aspect, near, far) * frame.view;
        frame.sun[0] = sun[0]; frame.sun[1] = sun[1]; frame.sun[2] = sun[2];

        auto shadow_pipeline = pipeline("shadow_vertex", nullptr, MTLPixelFormatInvalid, MTLPixelFormatDepth32Float);
        auto compare_desc = [MTLSamplerDescriptor new];
        compare_desc.minFilter = compare_desc.magFilter = MTLSamplerMinMagFilterLinear;
        compare_desc.compareFunction = MTLCompareFunctionLessEqual;
        compare_desc.sAddressMode = compare_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        auto compare = [device newSamplerStateWithDescriptor:compare_desc];
        auto depth_desc = [MTLDepthStencilDescriptor new];
        depth_desc.depthCompareFunction = MTLCompareFunctionLess;
        depth_desc.depthWriteEnabled = YES;
        auto depth_state = [device newDepthStencilStateWithDescriptor:depth_desc];
        auto scene_depth = [&] {
            auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:width height:height mipmapped:NO];
            desc.usage = MTLTextureUsageRenderTarget;
            desc.storageMode = MTLStorageModePrivate;
            return [device newTextureWithDescriptor:desc];
        }();
        const auto instances = NSUInteger(models.size());

        // Cascades: practical splits (lambda 0.75), each fitted by a bounding sphere so its size never changes.
        const auto fit_cascades = [&](uint32_t count) {
            frame.cascades = count;
            auto previous = near;
            for (uint32_t c = 0; c < count; ++c) {
                const auto p = float(c + 1) / float(count);
                const auto split = 0.75f * near * std::pow(far / near, p) + 0.25f * (near + (far - near) * p);
                // The frustum slice's bounding sphere, from its 8 corners.
                const auto t = std::tan(fov / 2);
                Vec3 corners[8];
                int k = 0;
                const auto inverse_view_dir = [&](float x, float y, float z) { // view space to world
                    const auto& v = frame.view.m;
                    const Vec3 vs = {x, y, -z};
                    const Vec3 translation = {v[12], v[13], v[14]};
                    const auto d = sub(vs, translation);
                    return Vec3{v[0] * d[0] + v[1] * d[1] + v[2] * d[2], v[4] * d[0] + v[5] * d[1] + v[6] * d[2],
                                v[8] * d[0] + v[9] * d[1] + v[10] * d[2]};
                };
                for (const auto z : {previous, split})
                    for (const auto sx : {-1.0f, 1.0f})
                        for (const auto sy : {-1.0f, 1.0f}) corners[k++] = inverse_view_dir(sx * z * t * aspect, sy * z * t, z);
                auto center = Vec3{0, 0, 0};
                for (const auto& corner : corners)
                    for (int i = 0; i < 3; ++i) center[i] += corner[i] / 8;
                auto radius = 0.0f;
                for (const auto& corner : corners) radius = std::max(radius, std::sqrt(dot(sub(corner, center), sub(corner, center))));
                const auto light_eye = Vec3{center[0] + sun[0] * 200, center[1] + sun[1] * 200, center[2] + sun[2] * 200};
                frame.cascade[c] = orthographic(radius, 0.1f, 400.0f) * look_at(light_eye, center, {0, 1, 0});
                frame.splits[c] = split;
                previous = split;
            }
        };

        const auto render = [&](MTLPixelFormat format, uint32_t cascades, uint32_t resolution, uint32_t taps, bool keep) {
            fit_cascades(cascades);
            frame.taps = taps;
            const auto atlas_size = resolution * 2;
            frame.texel = 1.0f / float(atlas_size);
            auto atlas_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:atlas_size
                                                                                height:atlas_size mipmapped:NO];
            atlas_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            atlas_desc.storageMode = MTLStorageModePrivate;
            auto atlas = [device newTextureWithDescriptor:atlas_desc];
            auto color = target(format, width, height, keep);
            auto lit_pipeline = pipeline("lit_vertex", "lit_fragment", format, MTLPixelFormatDepth32Float);
            auto shadow_times = std::vector<double>{}, lit_times = std::vector<double>{};
            for (int frame_index = 0; frame_index < 30; ++frame_index) {
                auto buffer = [queue commandBuffer];
                if (taps > 0) {
                    auto rp = [MTLRenderPassDescriptor new];
                    rp.depthAttachment.texture = atlas;
                    rp.depthAttachment.loadAction = MTLLoadActionClear;
                    rp.depthAttachment.clearDepth = 1.0;
                    rp.depthAttachment.storeAction = MTLStoreActionStore;
                    auto encoder = [buffer renderCommandEncoderWithDescriptor:rp];
                    [encoder setRenderPipelineState:shadow_pipeline];
                    [encoder setDepthStencilState:depth_state];
                    [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
                    [encoder setVertexBuffer:model_buffer offset:0 atIndex:1];
                    for (uint32_t c = 0; c < cascades; ++c) {
                        const auto viewport = MTLViewport{double(c % 2) * resolution, double(c / 2) * resolution, double(resolution),
                                                          double(resolution), 0, 1};
                        [encoder setViewport:viewport];
                        [encoder setVertexBytes:&frame.cascade[c] length:sizeof(Matrix) atIndex:2];
                        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36 instanceCount:instances];
                    }
                    [encoder endEncoding];
                    [buffer commit];
                    [buffer waitUntilCompleted];
                    shadow_times.push_back(gpu_ms(buffer));
                    buffer = [queue commandBuffer];
                }
                auto rp = [MTLRenderPassDescriptor new];
                rp.colorAttachments[0].texture = color;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0.4, 0.5, 0.6, 1);
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.depthAttachment.texture = scene_depth;
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = 1.0;
                rp.depthAttachment.storeAction = MTLStoreActionDontCare;
                auto encoder = [buffer renderCommandEncoderWithDescriptor:rp];
                [encoder setRenderPipelineState:lit_pipeline];
                [encoder setDepthStencilState:depth_state];
                [encoder setVertexBuffer:vertex_buffer offset:0 atIndex:0];
                [encoder setVertexBuffer:model_buffer offset:0 atIndex:1];
                [encoder setVertexBytes:&frame length:sizeof(frame) atIndex:2];
                [encoder setFragmentBytes:&frame length:sizeof(frame) atIndex:2];
                [encoder setFragmentTexture:atlas atIndex:0];
                [encoder setFragmentSamplerState:compare atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36 instanceCount:instances];
                [encoder endEncoding];
                [buffer commit];
                [buffer waitUntilCompleted];
                lit_times.push_back(gpu_ms(buffer));
            }
            std::ranges::sort(shadow_times);
            std::ranges::sort(lit_times);
            if (keep) write_png(color, output / "shadows.png");
            return std::pair{shadow_times.empty() ? 0.0 : shadow_times[shadow_times.size() / 2], lit_times[lit_times.size() / 2]};
        };

        std::printf("\nScene: a floor and 10,000 boxes at %lux%lu (GPU ms, median of 30 frames)\n", (unsigned long)width, (unsigned long)height);
        render(MTLPixelFormatRGBA8Unorm, 4, 2048, 9, false); // warm-up: the GPU ramps its clocks on the first frames
        for (const auto [format, label] : {std::pair{MTLPixelFormatRGBA8Unorm, "RGBA8"}, std::pair{MTLPixelFormatRGBA16Float, "RGBA16F"},
                                           std::pair{MTLPixelFormatRG11B10Float, "RG11B10F"}}) {
            const auto [shadow, lit] = render(format, 4, 2048, 0, false);
            (void)shadow;
            std::printf("  %-9s scene target, no shadows: %.3f ms\n", label, lit);
        }
        std::printf("  Cascaded shadows, RGBA16F target:\n");
        for (const auto cascades : {3u, 4u})
            for (const auto resolution : {1024u, 2048u})
                for (const auto taps : {1u, 9u, 25u}) {
                    const auto keep = cascades == 4 && resolution == 2048 && taps == 9;
                    const auto [shadow, lit] = render(keep ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA16Float, cascades, resolution, taps, keep);
                    std::printf("    %u cascades x %4u, %2u taps: shadow maps %.3f ms, lit pass %.3f ms%s\n", cascades, resolution, taps, shadow, lit,
                                keep ? "  (shadows.png, RGBA8)" : "");
                    // The chosen setup measured 0.67 ms; the bound leaves room for a busy machine.
                    if (keep && !(shadow + lit < 2.0)) unexpected("the chosen shadow setup took 2 ms or more");
                }
    }
    return failures == 0 ? 0 : 1;
}
