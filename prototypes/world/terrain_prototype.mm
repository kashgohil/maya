// Terrain and generator prototype for the world-scale decisions (#1060, docs/architecture/world-scale-decision.md).
//
// 1. Noise height field: seeded gradient-noise fBm with a domain warp, evaluated per tile from global
//    sample coordinates. Time per tile at 65, 129, and 257 samples a side; byte-identical tiles whatever
//    the thread count; and neighbouring tiles' shared edges equal.
// 2. Jolt height fields: build time and memory per tile, 1,000 downward ray casts, and a ball dropped on
//    the slope coming to rest on it.
// 3. Scatter: candidates on a grid of spacing-sized squares, one hashed point each (so the minimum spacing
//    holds by construction), filtered by density, slope, height, and an exclusion circle. Each item's ID
//    comes from the generator and its square, so changing a rule only removes or adds items: the
//    survivors keep their IDs and positions. Time per cell.
// 4. Rendering on Metal: a 4 km x 4 km terrain (2 m samples, one height texture), drawn with
//    geomipmapping (64 m tiles, one level per tile by distance) and with CDLOD (a quadtree of 32x32-quad
//    patches morphing between levels), from three viewpoints at 1920x1080. CPU selection time, patches,
//    triangles, and GPU time for each.
// Lines starting UNEXPECTED report what the decision relies on and make the run fail.

#include "world/jolt_scene.hpp"

#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
int failures = 0;
void unexpected(const std::string& what) {
    std::printf("UNEXPECTED: %s\n", what.c_str());
    ++failures;
}
uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
uint64_t fnv(const void* data, size_t size, uint64_t hash = 0xcbf29ce484222325ull) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

// --- 1. Noise ------------------------------------------------------------------------------------------
// Gradient noise on an integer lattice hashed with the seed: the same inputs give the same bits on every
// thread. Only float adds and multiplies in a fixed order; no FMA contraction across compilers is assumed,
// so cooked results are compared on one machine, and re-cooked rather than shared across platforms.
struct NoiseSettings {
    uint64_t seed = 990;
    int octaves = 6;
    double frequency = 1.0 / 900.0; // cycles per metre at the first octave
    float amplitude = 140.0f;       // metres
    float lacunarity = 2.0f, gain = 0.5f;
    float warp = 120.0f; // metres of domain warp
};
float gradient(uint64_t seed, int64_t ix, int64_t iz, float fx, float fz) {
    const auto h = mix(seed ^ mix(uint64_t(ix) * 0x8da6b343u ^ uint64_t(iz) * 0xd8163841u));
    const auto angle = float(h >> 40) * (6.2831853f / float(1u << 24));
    return std::cos(angle) * fx + std::sin(angle) * fz;
}
float smooth(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
float noise(uint64_t seed, double x, double z) {
    const auto x0 = std::floor(x), z0 = std::floor(z);
    const auto ix = int64_t(x0), iz = int64_t(z0);
    const auto fx = float(x - x0), fz = float(z - z0);
    const auto a = gradient(seed, ix, iz, fx, fz), b = gradient(seed, ix + 1, iz, fx - 1, fz);
    const auto c = gradient(seed, ix, iz + 1, fx, fz - 1), d = gradient(seed, ix + 1, iz + 1, fx - 1, fz - 1);
    const auto u = smooth(fx), v = smooth(fz);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}
float fbm(const NoiseSettings& s, uint64_t seed, double x, double z) {
    auto sum = 0.0f, amplitude = 1.0f;
    auto frequency = s.frequency;
    for (int o = 0; o < s.octaves; ++o) {
        sum += noise(seed + uint64_t(o), x * frequency, z * frequency) * amplitude;
        frequency *= double(s.lacunarity);
        amplitude *= s.gain;
    }
    return sum;
}
// Height at world sample (gx, gz) on a grid of `spacing` metres.
float height_at(const NoiseSettings& s, int64_t gx, int64_t gz, double spacing) {
    const auto x = double(gx) * spacing, z = double(gz) * spacing;
    const auto wx = x + double(s.warp * fbm(s, s.seed ^ 0x77, x, z)), wz = z + double(s.warp * fbm(s, s.seed ^ 0x99, x, z));
    return s.amplitude * fbm(s, s.seed, wx, wz);
}
// One tile: `samples` x `samples`, sharing its last row and column with the next tile.
std::vector<float> make_tile(const NoiseSettings& s, int tile_x, int tile_z, int samples, double spacing, int threads = 1) {
    auto tile = std::vector<float>(size_t(samples * samples));
    const auto rows = [&](int first, int last) {
        for (int z = first; z < last; ++z)
            for (int x = 0; x < samples; ++x)
                tile[size_t(z * samples + x)] = height_at(s, int64_t(tile_x) * (samples - 1) + x, int64_t(tile_z) * (samples - 1) + z, spacing);
    };
    if (threads == 1) {
        rows(0, samples);
    } else {
        auto workers = std::vector<std::thread>{};
        for (int t = 0; t < threads; ++t) workers.emplace_back(rows, samples * t / threads, samples * (t + 1) / threads);
        for (auto& w : workers) w.join();
    }
    return tile;
}

// --- 3. Scatter ----------------------------------------------------------------------------------------
struct ScatterRules {
    float spacing = 4.0f;       // metres between items, at least
    float density = 0.6f;       // fraction of candidates kept
    float max_slope = 30.0f;    // degrees
    float min_height = -40.0f, max_height = 90.0f;
    float exclude_x = 80, exclude_z = 80, exclude_radius = 20; // a clearing
};
struct Item {
    uint64_t id;
    float x, y, z, yaw, scale;
};
// Items in one 128 m cell; heights and slopes from the noise, as a cooked height tile would give them.
std::vector<Item> scatter(const NoiseSettings& noise_settings, const ScatterRules& rules, uint64_t generator, int cell_x, int cell_z, double spacing) {
    constexpr float cell = 128;
    auto items = std::vector<Item>{};
    const auto squares = int(cell / rules.spacing);
    for (int sz = 0; sz < squares; ++sz) {
        for (int sx = 0; sx < squares; ++sx) {
            // The square's global coordinates name the candidate: its ID never depends on the rules.
            const auto gx = int64_t(cell_x) * squares + sx, gz = int64_t(cell_z) * squares + sz;
            const auto h = mix(generator ^ mix(uint64_t(gx) * 0x9e3779b1u ^ uint64_t(gz) * 0x85ebca77u));
            const auto r = [&](int k) { return float(mix(h + uint64_t(k)) >> 40) / float(1u << 24); };
            if (r(0) >= rules.density) continue;
            // A point inside the square, keeping half the spacing from its edges: neighbours stay apart.
            const auto x = (float(gx) + 0.25f + 0.5f * r(1)) * rules.spacing, z = (float(gz) + 0.25f + 0.5f * r(2)) * rules.spacing;
            if (std::hypot(x - rules.exclude_x, z - rules.exclude_z) < rules.exclude_radius) continue;
            const auto sample = [&](float px, float pz) {
                return height_at(noise_settings, int64_t(std::floor(px / spacing)), int64_t(std::floor(pz / spacing)), spacing);
            };
            const auto y = sample(x, z);
            if (y < rules.min_height || y > rules.max_height) continue;
            const auto dx = (sample(x + float(spacing), z) - y) / float(spacing), dz = (sample(x, z + float(spacing)) - y) / float(spacing);
            if (std::atan(std::sqrt(dx * dx + dz * dz)) * 57.29578f > rules.max_slope) continue;
            items.push_back({h, x, y, z, r(3) * 6.2831853f, 0.8f + 0.4f * r(4)});
        }
    }
    return items;
}

// --- 4. Rendering --------------------------------------------------------------------------------------
constexpr float terrain_size = 4096, sample_spacing = 2;
constexpr int texture_size = 2049; // samples a side
constexpr int patch_quads = 32;    // a patch is 32 x 32 quads

struct Patch {
    float origin[2];
    float size;
    float morph_start, morph_end; // CDLOD: the distances over which odd vertices slide onto even ones
    float pad[3];
};
struct Uniforms {
    float view_projection[16];
    float camera[4];
    float terrain[4]; // size, height texture texels, vertical scale, quads per patch
};

const char* shader_source = R"(
#include <metal_stdlib>
using namespace metal;
struct Patch { float2 origin; float size; float morph_start; float morph_end; float pad0, pad1, pad2; };
struct Uniforms { float4x4 view_projection; float4 camera; float4 terrain; };
struct Out { float4 position [[position]]; float2 uv; };
float height_at(texture2d<float> heights, float2 world, float size) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return heights.sample(s, world / size, level(0)).r;
}
vertex Out terrain_vertex(uint vid [[vertex_id]], uint iid [[instance_id]], constant Patch* patches [[buffer(0)]],
                          constant Uniforms& u [[buffer(1)]], texture2d<float> heights [[texture(0)]]) {
    const uint side = uint(u.terrain.w) + 1;
    const float2 grid = float2(vid % side, vid / side);
    const Patch p = patches[iid];
    const float quad = p.size / u.terrain.w;
    float2 world = p.origin + grid * quad;
    const float d = distance(float3(world.x, height_at(heights, world, u.terrain.x), world.y), u.camera.xyz);
    const float morph = saturate((d - p.morph_start) / max(p.morph_end - p.morph_start, 1e-3));
    world -= fract(grid * 0.5) * 2.0 * quad * morph; // odd vertices slide onto their even neighbours
    Out out;
    out.position = u.view_projection * float4(world.x, height_at(heights, world, u.terrain.x), world.y, 1.0);
    out.uv = world / u.terrain.x;
    return out;
}
fragment float4 terrain_fragment(Out in [[stage_in]], constant Uniforms& u [[buffer(1)]], texture2d<float> heights [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    const float texel = 1.0 / u.terrain.y, step = u.terrain.x * texel;
    const float hx = heights.sample(s, in.uv + float2(texel, 0)).r - heights.sample(s, in.uv - float2(texel, 0)).r;
    const float hz = heights.sample(s, in.uv + float2(0, texel)).r - heights.sample(s, in.uv - float2(0, texel)).r;
    const float3 n = normalize(float3(-hx, 2.0 * step, -hz));
    const float light = saturate(dot(n, normalize(float3(0.4, 0.8, 0.3))));
    return float4(float3(0.35, 0.42, 0.25) * (0.15 + light), 1.0);
}
)";

struct Mat4 {
    float m[16]; // column-major
};
Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
    return r;
}
Mat4 view_projection(const float eye[3], const float target[3]) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const auto fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (auto& v : f) v /= fl;
    float s[3] = {-f[2], 0, f[0]}; // f x up
    const auto sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
    s[0] /= sl, s[2] /= sl;
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 view{};
    view.m[0] = s[0], view.m[4] = s[1], view.m[8] = s[2];
    view.m[1] = u[0], view.m[5] = u[1], view.m[9] = u[2];
    view.m[2] = -f[0], view.m[6] = -f[1], view.m[10] = -f[2];
    view.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    view.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    view.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    view.m[15] = 1;
    const auto t = 1.0f / std::tan(0.5236f), n = 0.1f, far = 6000.0f;
    Mat4 projection{};
    projection.m[0] = t / (16.0f / 9.0f), projection.m[5] = t, projection.m[10] = far / (n - far), projection.m[11] = -1;
    projection.m[14] = far * n / (n - far);
    return multiply(projection, view);
}
// Is a box (x, z range; all heights) at least partly inside the view? Clip-space test of its corners.
bool in_view(const Mat4& vp, float x0, float z0, float x1, float z1, float y0, float y1) {
    int outside[6] = {};
    for (int c = 0; c < 8; ++c) {
        const float p[4] = {c & 1 ? x1 : x0, c & 2 ? y1 : y0, c & 4 ? z1 : z0, 1};
        float clip[4] = {};
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) clip[r] += vp.m[k * 4 + r] * p[k];
        outside[0] += clip[0] < -clip[3], outside[1] += clip[0] > clip[3], outside[2] += clip[1] < -clip[3];
        outside[3] += clip[1] > clip[3], outside[4] += clip[2] < 0, outside[5] += clip[2] > clip[3];
    }
    for (const auto o : outside)
        if (o == 8) return false;
    return true;
}

struct Selection {
    std::vector<Patch> patches;
    double cpu_ms = 0;
};
// Geomipmapping: 64 m tiles of 32 quads; a tile's level halves its quads per doubling of distance past
// 128 m, the same density per distance as CDLOD's ranges below.
// Each level is drawn as a patch grid of 32 >> level quads; neighbours of different levels would need
// stitching or skirts, which this measurement leaves out.
std::vector<Selection> geomipmap(const Mat4& vp, const float eye[3], float y0, float y1) {
    auto levels = std::vector<Selection>(6);
    const auto start = Clock::now();
    constexpr float tile = 64;
    for (int tz = 0; tz < int(terrain_size / tile); ++tz) {
        for (int tx = 0; tx < int(terrain_size / tile); ++tx) {
            const auto x0 = float(tx) * tile, z0 = float(tz) * tile;
            if (!in_view(vp, x0, z0, x0 + tile, z0 + tile, y0, y1)) continue;
            const auto cx = std::clamp(eye[0], x0, x0 + tile), cz = std::clamp(eye[2], z0, z0 + tile);
            const auto d = std::hypot(cx - eye[0], cz - eye[2]);
            const auto level = d < 128.0f ? 0 : std::clamp(int(std::floor(std::log2(d / 128.0f))) + 1, 0, 5);
            levels[size_t(level)].patches.push_back({{x0, z0}, tile, 1e9f, 1e9f, {}});
        }
    }
    levels[0].cpu_ms = ms_since(start);
    return levels;
}
// CDLOD: a quadtree whose leaves are 64 m patches; level L covers distances up to 128 m * 2^L, and the
// last 30% of each range morphs into the next level.
Selection cdlod(const Mat4& vp, const float eye[3], float y0, float y1) {
    auto selection = Selection{};
    const auto start = Clock::now();
    constexpr int levels = 7; // 4096 m root down to 64 m leaves
    float ranges[levels];
    for (int l = 0; l < levels; ++l) ranges[l] = 128.0f * float(1 << l);
    const auto distance_to = [&](float x0, float z0, float size) {
        const auto cx = std::clamp(eye[0], x0, x0 + size), cz = std::clamp(eye[2], z0, z0 + size);
        const auto cy = std::clamp(eye[1], y0, y1);
        return std::sqrt((cx - eye[0]) * (cx - eye[0]) + (cy - eye[1]) * (cy - eye[1]) + (cz - eye[2]) * (cz - eye[2]));
    };
    const auto add = [&](float x0, float z0, float size, int level) {
        selection.patches.push_back({{x0, z0}, size, ranges[level] * 0.7f, ranges[level], {}});
    };
    // Returns false when the node is beyond its level's range (its parent draws the area instead).
    std::function<bool(float, float, float, int)> select = [&](float x0, float z0, float size, int level) {
        if (distance_to(x0, z0, size) > ranges[level]) return false;
        if (!in_view(vp, x0, z0, x0 + size, z0 + size, y0, y1)) return true;
        if (level == 0 || distance_to(x0, z0, size) > ranges[level - 1]) {
            add(x0, z0, size, level);
            return true;
        }
        const auto half = size * 0.5f;
        for (int c = 0; c < 4; ++c) {
            const auto cx = x0 + (c & 1 ? half : 0), cz = z0 + (c & 2 ? half : 0);
            if (!select(cx, cz, half, level - 1) && in_view(vp, cx, cz, cx + half, cz + half, y0, y1)) add(cx, cz, half, level);
        }
        return true;
    };
    select(0, 0, terrain_size, levels - 1);
    selection.cpu_ms = ms_since(start);
    return selection;
}

struct GpuResult {
    size_t patches = 0, triangles = 0;
    double cpu_ms = 0, gpu_ms = 0;
};
class Renderer {
public:
    explicit Renderer(const std::vector<float>& heights) {
        m_device = MTLCreateSystemDefaultDevice();
        m_queue = [m_device newCommandQueue];
        NSError* error = nil;
        auto library = [m_device newLibraryWithSource:@(shader_source) options:nil error:&error];
        if (!library) {
            unexpected(std::string("the terrain shader did not compile: ") + error.localizedDescription.UTF8String);
            return;
        }
        auto pipeline = [MTLRenderPipelineDescriptor new];
        pipeline.vertexFunction = [library newFunctionWithName:@"terrain_vertex"];
        pipeline.fragmentFunction = [library newFunctionWithName:@"terrain_fragment"];
        pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        m_pipeline = [m_device newRenderPipelineStateWithDescriptor:pipeline error:&error];
        auto depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction = MTLCompareFunctionLess;
        depth.depthWriteEnabled = YES;
        m_depth_state = [m_device newDepthStencilStateWithDescriptor:depth];
        auto texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:texture_size height:texture_size mipmapped:NO];
        m_heights = [m_device newTextureWithDescriptor:texture];
        [m_heights replaceRegion:MTLRegionMake2D(0, 0, texture_size, texture_size) mipmapLevel:0 withBytes:heights.data() bytesPerRow:texture_size * sizeof(float)];
        auto color = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1920 height:1080 mipmapped:NO];
        color.usage = MTLTextureUsageRenderTarget;
        color.storageMode = MTLStorageModePrivate;
        m_color = [m_device newTextureWithDescriptor:color];
        auto depth_texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:1920 height:1080 mipmapped:NO];
        depth_texture.usage = MTLTextureUsageRenderTarget;
        depth_texture.storageMode = MTLStorageModePrivate;
        m_depth = [m_device newTextureWithDescriptor:depth_texture];
        // Index buffers for patch grids of 32, 16, 8, 4, 2, and 1 quads, all over the same 33 x 33 vertices.
        for (int level = 0; level < 6; ++level) {
            const auto step = 1 << level;
            auto indices = std::vector<uint16_t>{};
            for (int z = 0; z < patch_quads; z += step)
                for (int x = 0; x < patch_quads; x += step) {
                    const auto v = [&](int dx, int dz) { return uint16_t((z + dz) * (patch_quads + 1) + x + dx); };
                    indices.insert(indices.end(), {v(0, 0), v(0, step), v(step, 0), v(step, 0), v(0, step), v(step, step)});
                }
            m_index_counts[level] = indices.size();
            m_indices[level] = [m_device newBufferWithBytes:indices.data() length:indices.size() * 2 options:MTLResourceStorageModeShared];
        }
    }
    bool ready() const { return m_pipeline != nil; }
    // Draws each list of patches with its level's index buffer; returns GPU time, the best of 20 frames.
    double draw(const std::vector<std::pair<int, const std::vector<Patch>*>>& lists, const float eye[3], const Mat4& vp) {
        auto best = 1e30;
        for (int frame = 0; frame < 20; ++frame) {
            auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = m_color;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.depthAttachment.texture = m_depth;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.storeAction = MTLStoreActionDontCare;
            pass.depthAttachment.clearDepth = 1.0;
            auto commands = [m_queue commandBuffer];
            auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:m_pipeline];
            [encoder setDepthStencilState:m_depth_state];
            [encoder setCullMode:MTLCullModeNone];
            auto uniforms = Uniforms{};
            std::memcpy(uniforms.view_projection, vp.m, sizeof vp.m);
            uniforms.camera[0] = eye[0], uniforms.camera[1] = eye[1], uniforms.camera[2] = eye[2];
            uniforms.terrain[0] = terrain_size, uniforms.terrain[1] = float(texture_size), uniforms.terrain[2] = 1, uniforms.terrain[3] = patch_quads;
            [encoder setVertexBytes:&uniforms length:sizeof uniforms atIndex:1];
            [encoder setFragmentBytes:&uniforms length:sizeof uniforms atIndex:1];
            [encoder setVertexTexture:m_heights atIndex:0];
            [encoder setFragmentTexture:m_heights atIndex:0];
            for (const auto& [level, patches] : lists) {
                if (patches->empty()) continue;
                auto buffer = [m_device newBufferWithBytes:patches->data() length:patches->size() * sizeof(Patch) options:MTLResourceStorageModeShared];
                [encoder setVertexBuffer:buffer offset:0 atIndex:0];
                [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:m_index_counts[level] indexType:MTLIndexTypeUInt16
                                   indexBuffer:m_indices[level] indexBufferOffset:0 instanceCount:patches->size()];
            }
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            best = std::min(best, (commands.GPUEndTime - commands.GPUStartTime) * 1000.0);
        }
        return best;
    }
    size_t triangles(int level) const { return m_index_counts[level] / 3; }

private:
    id<MTLDevice> m_device;
    id<MTLCommandQueue> m_queue;
    id<MTLRenderPipelineState> m_pipeline;
    id<MTLDepthStencilState> m_depth_state;
    id<MTLTexture> m_heights, m_color, m_depth;
    id<MTLBuffer> m_indices[6];
    size_t m_index_counts[6] = {};
};
} // namespace

int main() {
    @autoreleasepool {
        std::printf("Terrain and generator prototype (#1060)\n");
        const auto settings = NoiseSettings{};
        auto jolt = prototype::JoltRuntime{};

        std::printf("\n1. Noise height field: tiles of 128 m\n");
        for (const auto samples : {65, 129, 257}) {
            const auto spacing = 128.0 / (samples - 1);
            const auto start = Clock::now();
            const auto one = make_tile(settings, 3, 5, samples, spacing);
            const auto ms = ms_since(start);
            const auto threaded = make_tile(settings, 3, 5, samples, spacing, 8);
            const auto right = make_tile(settings, 4, 5, samples, spacing), below = make_tile(settings, 3, 6, samples, spacing);
            auto edges_equal = true;
            for (int i = 0; i < samples; ++i) {
                edges_equal &= one[size_t(i * samples + samples - 1)] == right[size_t(i * samples)];
                edges_equal &= one[size_t((samples - 1) * samples + i)] == below[size_t(i)];
            }
            const auto identical = fnv(one.data(), one.size() * 4) == fnv(threaded.data(), threaded.size() * 4);
            std::printf("   %3d samples (%.2f m): %7.2f ms a tile on one thread; 8 threads identical: %s; shared edges equal: %s\n", samples,
                spacing, ms, identical ? "yes" : "NO", edges_equal ? "yes" : "NO");
            if (!identical) unexpected("a tile differs with the thread count");
            if (!edges_equal) unexpected("neighbouring tiles disagree on their shared edge");
        }

        std::printf("\n2. Jolt height fields: one 128 m tile\n");
        for (const auto samples : {65, 129, 257}) {
            const auto spacing = 128.0f / float(samples - 1);
            const auto tile = make_tile(settings, 3, 5, samples, double(spacing), 8);
            auto start = Clock::now();
            auto shape_settings = JPH::HeightFieldShapeSettings(tile.data(), JPH::Vec3(0, 0, 0), JPH::Vec3(spacing, 1, spacing), uint32_t(samples));
            shape_settings.mBlockSize = 4;
            auto shape = shape_settings.Create();
            const auto build = ms_since(start);
            if (shape.HasError()) {
                unexpected(std::string("Jolt refused the height field: ") + shape.GetError().c_str());
                continue;
            }
            auto scene = prototype::PhysicsScene{};
            auto& bodies = scene.system.GetBodyInterface();
            bodies.CreateAndAddBody(JPH::BodyCreationSettings(shape.Get(), JPH::RVec3(0, 0, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                prototype::static_layer), JPH::EActivation::DontActivate);
            scene.system.OptimizeBroadPhase();
            // 1,000 downward rays; each must hit within a centimetre of the sampled height under it.
            start = Clock::now();
            auto worst = 0.0f;
            for (int i = 0; i < 1000; ++i) {
                const auto gx = int(mix(uint64_t(i)) % uint64_t(samples - 1)), gz = int(mix(uint64_t(i) ^ 0x5555) % uint64_t(samples - 1));
                const auto ray = JPH::RRayCast(JPH::RVec3(float(gx) * spacing, 1000, float(gz) * spacing), JPH::Vec3(0, -2000, 0));
                auto hit = JPH::RayCastResult{};
                if (!scene.system.GetNarrowPhaseQuery().CastRay(ray, hit)) {
                    worst = 1e9f;
                    continue;
                }
                worst = std::max(worst, std::abs(float(ray.GetPointOnRay(hit.mFraction).GetY()) - tile[size_t(gz * samples + gx)]));
            }
            const auto rays = ms_since(start);
            // A box dropped on the middle of the tile lands on it and stays above it.
            const auto middle = tile[size_t((samples / 2) * samples + samples / 2)];
            auto box_settings = JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f)), JPH::RVec3(64, middle + 3, 64),
                JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, prototype::moving_layer);
            box_settings.mFriction = 1.0f;
            const auto ball = bodies.CreateAndAddBody(box_settings, JPH::EActivation::Activate);
            auto jobs = JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 2);
            for (int t = 0; t < 900; ++t) scene.system.Update(1.0f / 60.0f, 1, &scene.temp, &jobs);
            const auto at = bodies.GetPosition(ball);
            const auto above = float(at.GetY()) - height_at(settings, int64_t(3 * (samples - 1)) + int64_t(float(at.GetX()) / spacing),
                int64_t(5 * (samples - 1)) + int64_t(float(at.GetZ()) / spacing), double(spacing));
            const auto resting = !bodies.IsActive(ball) || bodies.GetLinearVelocity(ball).Length() < 0.05f;
            std::printf("   %3d samples: built in %6.2f ms, %7.1f KiB; 1,000 rays %.3f ms, worst %.4f m off; box %.2f m above the ground, %s\n",
                samples, build, double(shape.Get()->GetStats().mSizeBytes) / 1024, rays, double(worst), double(above),
                resting ? "at rest" : "still moving");
            if (worst > 0.05f) unexpected("height-field rays missed the sampled height by " + std::to_string(worst) + " m");
            if (above < 0.0f || above > 2.0f) unexpected("the box left the height field: " + std::to_string(above) + " m above it");
        }

        std::printf("\n3. Scatter: one 128 m cell, 4 m spacing\n");
        {
            const auto generator = uint64_t(0x7363617474657231);
            auto rules = ScatterRules{};
            auto start = Clock::now();
            const auto items = scatter(settings, rules, generator, 0, 0, 2.0);
            const auto ms = ms_since(start);
            std::printf("   %zu items from %d candidates in %.2f ms\n", items.size(), int(128 / rules.spacing) * int(128 / rules.spacing), ms);
            auto closest = 1e9f;
            for (size_t a = 0; a < items.size(); ++a)
                for (size_t b = a + 1; b < items.size(); ++b) closest = std::min(closest, std::hypot(items[a].x - items[b].x, items[a].z - items[b].z));
            std::printf("   closest pair %.2f m apart (at least %.2f m required)\n", double(closest), double(rules.spacing) * 0.5);
            if (closest < rules.spacing * 0.5f) unexpected("scatter broke its minimum spacing");
            // Tighter rules only remove items; every survivor keeps its ID and position.
            auto by_id = std::unordered_map<uint64_t, Item>{};
            for (const auto& item : items) by_id[item.id] = item;
            for (const auto& [what, change] : std::initializer_list<std::pair<const char*, void (*)(ScatterRules&)>>{
                     {"slope limit 30 -> 20 degrees", [](ScatterRules& r) { r.max_slope = 20; }},
                     {"density 0.6 -> 0.3", [](ScatterRules& r) { r.density = 0.3f; }},
                     {"clearing radius 20 -> 40 m", [](ScatterRules& r) { r.exclude_radius = 40; }}}) {
                auto changed = rules;
                change(changed);
                const auto after = scatter(settings, changed, generator, 0, 0, 2.0);
                auto kept = size_t{0}, moved = size_t{0}, unknown = size_t{0};
                for (const auto& item : after) {
                    const auto found = by_id.find(item.id);
                    if (found == by_id.end()) ++unknown;
                    else if (found->second.x != item.x || found->second.z != item.z || found->second.y != item.y) ++moved;
                    else ++kept;
                }
                std::printf("   %-30s %4zu of %4zu items remain, %zu moved, %zu new\n", what, kept, items.size(), moved, unknown);
                if (moved || unknown) unexpected(std::string("changing the ") + what + " moved or renamed items");
            }
            const auto other = scatter(settings, rules, generator, 1, 0, 2.0);
            auto collide = size_t{0};
            for (const auto& item : other) collide += by_id.count(item.id);
            if (collide) unexpected("two cells produced the same item IDs");
        }

        std::printf("\n4. Rendering a 4 km x 4 km terrain, 2 m samples, 1920x1080\n");
        auto heights = std::vector<float>(size_t(texture_size) * texture_size);
        {
            const auto start = Clock::now();
            auto workers = std::vector<std::thread>{};
            const auto threads = int(std::thread::hardware_concurrency());
            for (int t = 0; t < threads; ++t)
                workers.emplace_back([&, t] {
                    for (int z = texture_size * t / threads; z < texture_size * (t + 1) / threads; ++z)
                        for (int x = 0; x < texture_size; ++x) heights[size_t(z * texture_size + x)] = height_at(settings, x, z, sample_spacing);
                });
            for (auto& w : workers) w.join();
            std::printf("   %d x %d samples generated in %.0f ms on %d threads\n", texture_size, texture_size, ms_since(start), threads);
        }
        const auto [low, high] = std::minmax_element(heights.begin(), heights.end());
        auto renderer = Renderer(heights);
        if (!renderer.ready()) return 1;
        struct Viewpoint {
            const char* name;
            float eye[3], target[3];
        };
        const Viewpoint viewpoints[] = {{"ground, looking across", {2048, 0, 2048}, {3500, 0, 2900}},
            {"hilltop, 60 m up", {1024, 60, 1024}, {3000, 0, 2500}}, {"aerial, 400 m up", {500, 400, 500}, {2500, 0, 2500}}};
        std::printf("   %-24s %-14s %8s %11s %9s %8s\n", "viewpoint", "scheme", "patches", "triangles", "cpu ms", "gpu ms");
        for (const auto& v : viewpoints) {
            float eye[3] = {v.eye[0], v.eye[1], v.eye[2]}, target[3] = {v.target[0], v.target[1], v.target[2]};
            const auto ground = heights[size_t(int(eye[2] / sample_spacing) * texture_size + int(eye[0] / sample_spacing))];
            eye[1] += ground + 1.7f;
            target[1] = ground;
            const auto vp = view_projection(eye, target);
            const auto mip = geomipmap(vp, eye, *low, *high);
            auto lists = std::vector<std::pair<int, const std::vector<Patch>*>>{};
            auto patches = size_t{0}, triangles = size_t{0};
            for (int level = 0; level < 6; ++level) {
                lists.push_back({level, &mip[size_t(level)].patches});
                patches += mip[size_t(level)].patches.size();
                triangles += mip[size_t(level)].patches.size() * renderer.triangles(level);
            }
            std::printf("   %-24s %-14s %8zu %11zu %9.3f %8.3f\n", v.name, "geomipmapping", patches, triangles, mip[0].cpu_ms, renderer.draw(lists, eye, vp));
            const auto quad = cdlod(vp, eye, *low, *high);
            std::printf("   %-24s %-14s %8zu %11zu %9.3f %8.3f\n", "", "CDLOD", quad.patches.size(), quad.patches.size() * renderer.triangles(0),
                quad.cpu_ms, renderer.draw({{0, &quad.patches}}, eye, vp));
            if (quad.patches.empty()) unexpected(std::string("CDLOD selected nothing from ") + v.name);
        }
    }
    if (failures) std::printf("\n%d unexpected results\n", failures);
    return failures ? 1 : 0;
}
