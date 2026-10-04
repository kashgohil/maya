#include "maya/rhi/metal/metal_device.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>
#import <AppKit/NSScreen.h>
#import <AppKit/NSView.h>
#import <AppKit/NSWindow.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace maya {

std::unique_ptr<GraphicsDevice> GraphicsDevice::create_default() {
    return std::make_unique<MetalDevice>();
}

namespace {
MTLPixelFormat pixel_format(Format format) {
    switch (format) {
    case Format::rgba8_unorm: return MTLPixelFormatRGBA8Unorm;
    case Format::rgba8_srgb: return MTLPixelFormatRGBA8Unorm_sRGB;
    case Format::bgra8_unorm: return MTLPixelFormatBGRA8Unorm;
    case Format::bgra8_srgb: return MTLPixelFormatBGRA8Unorm_sRGB;
    case Format::rgba16_float: return MTLPixelFormatRGBA16Float;
    case Format::depth32_float: return MTLPixelFormatDepth32Float;
    case Format::astc_4x4_unorm: return MTLPixelFormatASTC_4x4_LDR;
    case Format::astc_4x4_srgb: return MTLPixelFormatASTC_4x4_sRGB;
    case Format::astc_6x6_unorm: return MTLPixelFormatASTC_6x6_LDR;
    case Format::astc_6x6_srgb: return MTLPixelFormatASTC_6x6_sRGB;
    case Format::undefined: break;
    }
    return MTLPixelFormatInvalid;
}
MTLLoadAction load_action(LoadAction action) {
    switch (action) {
    case LoadAction::load: return MTLLoadActionLoad;
    case LoadAction::clear: return MTLLoadActionClear;
    case LoadAction::dont_care: break;
    }
    return MTLLoadActionDontCare;
}
MTLStoreAction store_action(StoreAction action) {
    return action == StoreAction::store ? MTLStoreActionStore : MTLStoreActionDontCare;
}
MTLCompareFunction compare_function(CompareFunction compare) {
    switch (compare) {
    case CompareFunction::never: return MTLCompareFunctionNever;
    case CompareFunction::less: return MTLCompareFunctionLess;
    case CompareFunction::less_equal: return MTLCompareFunctionLessEqual;
    case CompareFunction::equal: return MTLCompareFunctionEqual;
    case CompareFunction::greater: return MTLCompareFunctionGreater;
    case CompareFunction::greater_equal: return MTLCompareFunctionGreaterEqual;
    case CompareFunction::always: break;
    }
    return MTLCompareFunctionAlways;
}
MTLSamplerAddressMode address_mode(AddressMode mode) {
    switch (mode) {
    case AddressMode::repeat: return MTLSamplerAddressModeRepeat;
    case AddressMode::clamp_to_edge: return MTLSamplerAddressModeClampToEdge;
    case AddressMode::mirror_repeat: break;
    }
    return MTLSamplerAddressModeMirrorRepeat;
}
NSString* ns_string(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()]; }
std::string error_text(NSError* error) {
    const char* text = error ? error.localizedDescription.UTF8String : nullptr;
    return text ? text : "unknown error";
}
template<class T> void store(std::vector<T>& values, uint32_t slot, T value) {
    if (values.size() <= slot) values.resize(size_t{slot} + 1);
    values[slot] = value;
}
} // namespace

struct MetalPipeline {
    id<MTLRenderPipelineState> state = nil;
    id<MTLDepthStencilState> depth = nil;
    MTLCullMode cull = MTLCullModeBack;
    MTLWinding winding = MTLWindingCounterClockwise;
};

struct MetalDevice::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
    NSView* view = nil;
    id<MTLCommandBuffer> frame = nil;
    id<MTLCommandBuffer> last_submission = nil;
    std::deque<std::pair<uint64_t, id<MTLCommandBuffer>>> in_flight; // bounded by frames_in_flight
    id<MTLRenderCommandEncoder> encoder = nil;
    id<CAMetalDrawable> drawable = nil;
    std::vector<id<MTLBuffer>> buffers;
    std::vector<id<MTLTexture>> textures;
    std::vector<id<MTLSamplerState>> samplers;
    std::vector<MetalPipeline> pipelines;
    // Keeps the most recent transfer ordered before later frames and alive for wait_idle.
    id<MTLCommandBuffer> last_transfer = nil;

    // Pass timing (#1026): timestamps at each pass's vertex and fragment boundaries, four samples per
    // pass, in one sample buffer per frame slot. A slot is resolved on this thread once its frame is
    // known complete: when its timing is taken, or before the slot is reused.
    std::string pass_reason = "the device is not initialized"; // empty when passes can be timed
    std::vector<id<MTLCounterSampleBuffer>> sample_buffers;
    struct PassFrame {
        uint64_t serial = 0;
        std::vector<std::string> labels; // timed passes, in order
        uint32_t untimed = 0;
        bool pending = false; // encoded with timing and not yet resolved
    };
    std::vector<PassFrame> pass_frames; // per slot
    PassFrame encoding; // the frame being encoded
    struct ResolvedPass {
        std::string label;
        double vertex_start = NAN, vertex_end = NAN, fragment_start = NAN, fragment_end = NAN; // seconds; NaN: did not run
    };
    struct ResolvedFrame {
        std::vector<ResolvedPass> passes;
        uint32_t untimed = 0;
    };
    std::map<uint64_t, ResolvedFrame> resolved; // by serial, until attached to its timing
    // GPU timestamps to seconds on the clock of MTLCommandBuffer.GPUStartTime.
    MTLTimestamp cpu_base = 0, gpu_base = 0;
    double ns_per_tick = 1.0;
    std::chrono::steady_clock::time_point calibrated{};

    void calibrate() {
        MTLTimestamp cpu = 0, gpu = 0;
        [device sampleTimestamps:&cpu gpuTimestamp:&gpu];
        if (gpu_base == 0) cpu_base = cpu, gpu_base = gpu;
        else if (gpu > gpu_base + 100'000'000 && cpu > cpu_base) ns_per_tick = double(cpu - cpu_base) / double(gpu - gpu_base);
        calibrated = std::chrono::steady_clock::now();
    }
    double seconds(MTLTimestamp gpu) const {
        return (double(cpu_base) + (double(gpu) - double(gpu_base)) * ns_per_tick) / 1e9;
    }
    /// Reads a completed frame's samples from its slot into `resolved`.
    void resolve(uint32_t slot) {
        auto& frame = pass_frames[slot];
        if (!frame.pending) return;
        frame.pending = false;
        if (std::chrono::steady_clock::now() - calibrated > std::chrono::seconds(1)) calibrate();
        auto result = ResolvedFrame{{}, frame.untimed};
        if (!frame.labels.empty()) {
            NSData* data = [sample_buffers[slot] resolveCounterRange:NSMakeRange(0, frame.labels.size() * 4)];
            const auto* samples = data ? static_cast<const MTLCounterResultTimestamp*>(data.bytes) : nullptr;
            const auto count = data ? data.length / sizeof(MTLCounterResultTimestamp) : 0;
            const auto at = [&](size_t index) {
                return index < count && samples[index].timestamp != MTLCounterErrorValue && samples[index].timestamp != 0
                    ? seconds(samples[index].timestamp) : NAN;
            };
            for (size_t pass = 0; pass < frame.labels.size(); ++pass)
                result.passes.push_back({std::move(frame.labels[pass]), at(pass * 4), at(pass * 4 + 1), at(pass * 4 + 2), at(pass * 4 + 3)});
        }
        frame.labels.clear();
        resolved[frame.serial] = std::move(result);
        while (resolved.size() > RhiCompletion::timing_capacity) resolved.erase(resolved.begin());
    }
};

MetalDevice::MetalDevice() : m_impl(std::make_unique<Impl>()) {}

MetalDevice::~MetalDevice() {
    shutdown();
}

size_t MetalDevice::native_buffer_count() const noexcept {
    return static_cast<size_t>(std::ranges::count_if(m_impl->buffers, [](id<MTLBuffer> value) { return value != nil; }));
}
size_t MetalDevice::native_texture_count() const noexcept {
    return static_cast<size_t>(std::ranges::count_if(m_impl->textures, [](id<MTLTexture> value) { return value != nil; }));
}

bool MetalDevice::backend_initialize(void* native_window, RhiLimits& limits, Format& surface_format) {
    @autoreleasepool {
        m_impl->device = MTLCreateSystemDefaultDevice();
        if (!m_impl->device) return false;
        m_impl->queue = [m_impl->device newCommandQueue];
        if (!m_impl->queue) return false;
        m_impl->queue.label = @"Maya queue";
        limits.max_buffer_size = std::min<size_t>(m_impl->device.maxBufferLength, limits.max_buffer_size);
        limits.astc = [m_impl->device supportsFamily:MTLGPUFamilyApple2];
        m_impl->pass_reason.clear();
        id<MTLCounterSet> timestamps = nil;
        for (id<MTLCounterSet> set in m_impl->device.counterSets)
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) timestamps = set;
        if (!timestamps) m_impl->pass_reason = "this GPU has no timestamp counters";
        else if (![m_impl->device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
            m_impl->pass_reason = "this GPU cannot sample timestamps at render stage boundaries";
        for (uint32_t slot = 0; m_impl->pass_reason.empty() && slot < options().frames_in_flight; ++slot) {
            auto* descriptor = [MTLCounterSampleBufferDescriptor new];
            descriptor.counterSet = timestamps;
            descriptor.storageMode = MTLStorageModeShared;
            descriptor.sampleCount = max_timed_passes * 4;
            descriptor.label = @"Maya pass timestamps";
            NSError* error = nil;
            id<MTLCounterSampleBuffer> buffer = [m_impl->device newCounterSampleBufferWithDescriptor:descriptor error:&error];
            if (!buffer) m_impl->pass_reason = "Metal could not create a timestamp sample buffer: " + error_text(error);
            else m_impl->sample_buffers.push_back(buffer);
        }
        if (!m_impl->pass_reason.empty()) m_impl->sample_buffers.clear();
        m_impl->pass_frames.assign(m_impl->sample_buffers.size(), {});
        m_impl->gpu_base = 0;
        m_impl->ns_per_tick = 1.0;
        if (m_impl->pass_reason.empty()) m_impl->calibrate();
        if (native_window) {
            NSWindow* window = (__bridge NSWindow*)native_window;
            m_impl->view = window.contentView;
            m_impl->layer = [CAMetalLayer layer];
            m_impl->layer.device = m_impl->device;
            m_impl->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            m_impl->layer.framebufferOnly = YES;
            m_impl->view.layer = m_impl->layer;
            m_impl->view.wantsLayer = YES;
            const auto bounds = m_impl->view.bounds;
            const auto scale = window.backingScaleFactor;
            // A layer attached by hand is not scaled by AppKit: without this, a Retina display shows
            // the drawable as 1x content, softened and blocky.
            m_impl->layer.contentsScale = scale;
            m_impl->layer.opaque = YES;
            m_impl->layer.drawableSize = CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
            surface_format = Format::bgra8_unorm;
        }
        return true;
    }
}

void MetalDevice::backend_shutdown() noexcept {
    @autoreleasepool {
        m_impl->buffers.clear();
        m_impl->textures.clear();
        m_impl->samplers.clear();
        m_impl->pipelines.clear();
        m_impl->last_submission = nil;
        m_impl->in_flight.clear();
        m_impl->last_transfer = nil;
        m_impl->sample_buffers.clear();
        m_impl->pass_frames.clear();
        m_impl->encoding = {};
        m_impl->resolved.clear();
        m_impl->pass_reason = "the device is not initialized";
        if (m_impl->layer && m_impl->view.layer == m_impl->layer) m_impl->view.layer = nil;
        m_impl->view = nil;
        m_impl->layer = nil;
        m_impl->queue = nil;
        m_impl->device = nil;
    }
}

void MetalDevice::backend_resize(uint32_t width, uint32_t height) {
    if (!m_impl->layer) return;
    // Framebuffer size changes include moving to a display with another scale.
    if (auto* window = m_impl->view.window) m_impl->layer.contentsScale = window.backingScaleFactor;
    m_impl->layer.drawableSize = CGSizeMake(width, height);
}

double MetalDevice::surface_scale() const noexcept {
    return m_impl->layer ? m_impl->layer.contentsScale : 0.0;
}

RhiDiagnostic MetalDevice::backend_create_buffer(uint32_t slot, const BufferDesc& desc, const void* data) {
    @autoreleasepool {
        id<MTLBuffer> buffer = data
            ? [m_impl->device newBufferWithBytes:data length:desc.size options:MTLResourceStorageModeShared]
            : [m_impl->device newBufferWithLength:desc.size options:MTLResourceStorageModeShared];
        if (!buffer) return {RhiError::out_of_memory, "Metal could not allocate a " + std::to_string(desc.size) + "-byte buffer"};
        if (!data) std::memset(buffer.contents, 0, desc.size);
        if (!desc.label.empty()) buffer.label = ns_string(desc.label);
        store(m_impl->buffers, slot, buffer);
        return {};
    }
}

RhiDiagnostic MetalDevice::backend_create_texture(uint32_t slot, const TextureDesc& desc, const void* data) {
    @autoreleasepool {
        auto* descriptor = desc.type == TextureType::cube
            ? [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:pixel_format(desc.format) size:desc.width mipmapped:NO]
            : [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format(desc.format)
                                                                 width:desc.width
                                                                height:desc.height
                                                             mipmapped:NO];
        descriptor.mipmapLevelCount = desc.mip_levels;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageUnknown;
        if (has_flag(desc.usage, TextureUsage::sampled)) descriptor.usage |= MTLTextureUsageShaderRead;
        if (has_flag(desc.usage, TextureUsage::render_target)) descriptor.usage |= MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [m_impl->device newTextureWithDescriptor:descriptor];
        if (!texture) return {RhiError::out_of_memory, "Metal could not allocate a " + std::to_string(desc.width) + "x" +
            std::to_string(desc.height) + " " + format_name(desc.format) + " texture"};
        if (!desc.label.empty()) texture.label = ns_string(desc.label);
        if (data) {
            // Private textures are filled through a staging copy ordered before later frames: every
            // level from one buffer, level 0 first, rows of pixels or of compressed blocks.
            id<MTLBuffer> staging = [m_impl->device newBufferWithBytes:data length:texture_bytes(desc)
                                                               options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> upload = [m_impl->queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [upload blitCommandEncoder];
            if (!staging || !upload || !blit) return {RhiError::out_of_memory, "Metal could not stage texture data"};
            const auto block = block_extent(desc.format);
            auto offset = size_t{0};
            for (uint32_t level = 0; level < desc.mip_levels; ++level) {
                const auto width = mip_extent(desc.width, level), height = mip_extent(desc.height, level);
                const auto row = (size_t{width} + block - 1) / block * block_bytes(desc.format);
                const auto bytes = mip_level_bytes(desc.format, desc.width, desc.height, level);
                for (uint32_t face = 0; face < texture_faces(desc); ++face) { // a cube's faces are its slices
                    [blit copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:row sourceBytesPerImage:bytes
                              sourceSize:MTLSizeMake(width, height, 1) toTexture:texture destinationSlice:face
                        destinationLevel:level destinationOrigin:MTLOriginMake(0, 0, 0)];
                    offset += bytes;
                }
            }
            [blit endEncoding];
            upload.label = @"Maya texture upload";
            [upload commit];
            m_impl->last_transfer = upload;
        }
        store(m_impl->textures, slot, texture);
        return {};
    }
}

RhiDiagnostic MetalDevice::backend_create_sampler(uint32_t slot, const SamplerDesc& desc) {
    @autoreleasepool {
        auto* descriptor = [[MTLSamplerDescriptor alloc] init];
        descriptor.minFilter = desc.min_filter == Filter::linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        descriptor.magFilter = desc.mag_filter == Filter::linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        descriptor.mipFilter = desc.mip_filter == MipFilter::linear    ? MTLSamplerMipFilterLinear
                               : desc.mip_filter == MipFilter::nearest ? MTLSamplerMipFilterNearest
                                                                       : MTLSamplerMipFilterNotMipmapped;
        descriptor.maxAnisotropy = desc.max_anisotropy;
        descriptor.sAddressMode = address_mode(desc.address_u);
        descriptor.tAddressMode = address_mode(desc.address_v);
        if (!desc.label.empty()) descriptor.label = ns_string(desc.label);
        id<MTLSamplerState> sampler = [m_impl->device newSamplerStateWithDescriptor:descriptor];
        if (!sampler) return {RhiError::out_of_memory, "Metal could not create a sampler"};
        store(m_impl->samplers, slot, sampler);
        return {};
    }
}

RhiDiagnostic MetalDevice::backend_create_pipeline(uint32_t slot, const PipelineDesc& desc) {
    @autoreleasepool {
        const auto name = desc.label.empty() ? std::string("Pipeline") : "Pipeline '" + desc.label + "'";
        NSError* error = nil;
        id<MTLLibrary> library = [m_impl->device newLibraryWithSource:ns_string(desc.shader_source) options:nil error:&error];
        if (!library) return {RhiError::shader_compilation, name + ": shader compilation failed: " + error_text(error)};
        id<MTLFunction> vertex = [library newFunctionWithName:ns_string(desc.vertex_entry)];
        id<MTLFunction> fragment = [library newFunctionWithName:ns_string(desc.fragment_entry)];
        if (!vertex) return {RhiError::invalid_descriptor, name + ": vertex entry point '" + desc.vertex_entry + "' was not found"};
        if (!fragment) return {RhiError::invalid_descriptor, name + ": fragment entry point '" + desc.fragment_entry + "' was not found"};
        auto* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        for (size_t i = 0; i < desc.color_formats.size(); ++i) {
            auto* attachment = descriptor.colorAttachments[i];
            attachment.pixelFormat = pixel_format(desc.color_formats[i]);
            if (desc.blend == BlendMode::alpha) {
                attachment.blendingEnabled = YES;
                attachment.rgbBlendOperation = MTLBlendOperationAdd;
                attachment.alphaBlendOperation = MTLBlendOperationAdd;
                attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
                attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            }
        }
        descriptor.depthAttachmentPixelFormat = pixel_format(desc.depth_format);
        if (!desc.label.empty()) descriptor.label = ns_string(desc.label);
        auto pipeline = MetalPipeline{};
        pipeline.state = [m_impl->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline.state) return {RhiError::invalid_descriptor, name + ": Metal rejected the pipeline: " + error_text(error)};
        auto* depth = [[MTLDepthStencilDescriptor alloc] init];
        depth.depthCompareFunction = desc.depth.test ? compare_function(desc.depth.compare) : MTLCompareFunctionAlways;
        depth.depthWriteEnabled = desc.depth.write;
        pipeline.depth = [m_impl->device newDepthStencilStateWithDescriptor:depth];
        if (!pipeline.depth) return {RhiError::out_of_memory, name + ": Metal could not create depth state"};
        pipeline.cull = desc.cull == CullMode::none ? MTLCullModeNone : desc.cull == CullMode::front ? MTLCullModeFront : MTLCullModeBack;
        pipeline.winding = desc.front_face == Winding::clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
        store(m_impl->pipelines, slot, pipeline);
        return {};
    }
}

void MetalDevice::backend_release(ResourceKind kind, uint32_t slot) noexcept {
    switch (kind) {
    case ResourceKind::buffer: if (slot < m_impl->buffers.size()) m_impl->buffers[slot] = nil; break;
    case ResourceKind::texture: if (slot < m_impl->textures.size()) m_impl->textures[slot] = nil; break;
    case ResourceKind::sampler: if (slot < m_impl->samplers.size()) m_impl->samplers[slot] = nil; break;
    case ResourceKind::pipeline: if (slot < m_impl->pipelines.size()) m_impl->pipelines[slot] = {}; break;
    }
}

void MetalDevice::backend_write_buffer(uint32_t slot, size_t offset, const void* data, size_t size) noexcept {
    std::memcpy(static_cast<std::byte*>(m_impl->buffers[slot].contents) + offset, data, size);
}

RhiDiagnostic MetalDevice::backend_read_texture(uint32_t slot, const TextureDesc& desc, std::vector<std::byte>& pixels) {
    @autoreleasepool {
        const auto row = size_t{desc.width} * bytes_per_pixel(desc.format);
        id<MTLBuffer> staging = [m_impl->device newBufferWithLength:row * desc.height options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> copy = [m_impl->queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [copy blitCommandEncoder];
        if (!staging || !copy || !blit) return {RhiError::out_of_memory, "Metal could not allocate readback storage"};
        [blit copyFromTexture:m_impl->textures[slot] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(desc.width, desc.height, 1) toBuffer:staging destinationOffset:0
          destinationBytesPerRow:row destinationBytesPerImage:row * desc.height];
        [blit endEncoding];
        copy.label = @"Maya texture readback";
        [copy commit];
        [copy waitUntilCompleted];
        if (copy.status != MTLCommandBufferStatusCompleted)
            return {RhiError::gpu_failure, "Texture readback failed: " + error_text(copy.error)};
        const auto* bytes = static_cast<const std::byte*>(staging.contents);
        pixels.assign(bytes, bytes + row * desc.height);
        return {};
    }
}

RhiDiagnostic MetalDevice::backend_begin_frame() {
    // Resources are not retained by frame command buffers; deferred retirement owns their lifetime.
    m_impl->frame = [m_impl->queue commandBufferWithUnretainedReferences];
    if (!m_impl->frame) return {RhiError::device_unavailable, "Metal could not create a frame command buffer"};
    m_impl->frame.label = @"Maya frame";
    m_impl->encoding = {};
    if (frame_pass_timing() && !m_impl->sample_buffers.empty()) {
        // This frame's slot was last used frames_in_flight frames ago; that frame has completed.
        const auto serial = encoding_frame_serial();
        m_impl->resolve(uint32_t(serial % m_impl->sample_buffers.size()));
        m_impl->encoding.serial = serial;
        m_impl->encoding.pending = true;
    }
    return {};
}

GraphicsDevice::BackendSurface MetalDevice::backend_acquire_surface(uint32_t slot) {
    @autoreleasepool {
        const auto size = m_impl->layer.drawableSize;
        if (size.width < 1 || size.height < 1)
            return {0, 0, {RhiError::surface_unavailable, "Surface has zero size; skip presentation this frame"}};
        id<CAMetalDrawable> drawable = [m_impl->layer nextDrawable];
        if (!drawable)
            return {0, 0, {RhiError::surface_unavailable, "No drawable was available within the timeout; skip presentation this frame"}};
        m_impl->drawable = drawable;
        store(m_impl->textures, slot, drawable.texture);
        return {static_cast<uint32_t>(drawable.texture.width), static_cast<uint32_t>(drawable.texture.height), {}};
    }
}

RhiDiagnostic MetalDevice::backend_begin_pass(const RenderPassDesc& desc) {
    @autoreleasepool {
        auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        for (size_t i = 0; i < desc.colors.size(); ++i) {
            const auto& color = desc.colors[i];
            pass.colorAttachments[i].texture = m_impl->textures[color.texture.slot];
            pass.colorAttachments[i].loadAction = load_action(color.load);
            pass.colorAttachments[i].storeAction = store_action(color.store);
            pass.colorAttachments[i].clearColor = MTLClearColorMake(color.clear_color[0], color.clear_color[1],
                                                                    color.clear_color[2], color.clear_color[3]);
        }
        if (desc.depth) {
            pass.depthAttachment.texture = m_impl->textures[desc.depth->texture.slot];
            pass.depthAttachment.loadAction = load_action(desc.depth->load);
            pass.depthAttachment.storeAction = store_action(desc.depth->store);
            pass.depthAttachment.clearDepth = desc.depth->clear_depth;
        }
        if (m_impl->encoding.pending) {
            const auto index = frame_pass_index();
            if (index < max_timed_passes) {
                auto* attachment = pass.sampleBufferAttachments[0];
                attachment.sampleBuffer = m_impl->sample_buffers[m_impl->encoding.serial % m_impl->sample_buffers.size()];
                attachment.startOfVertexSampleIndex = index * 4;
                attachment.endOfVertexSampleIndex = index * 4 + 1;
                attachment.startOfFragmentSampleIndex = index * 4 + 2;
                attachment.endOfFragmentSampleIndex = index * 4 + 3;
                m_impl->encoding.labels.push_back(desc.label.empty() ? "pass " + std::to_string(index + 1) : desc.label);
            } else {
                ++m_impl->encoding.untimed;
            }
        }
        m_impl->encoder = [m_impl->frame renderCommandEncoderWithDescriptor:pass];
        if (!m_impl->encoder) return {RhiError::device_unavailable, "Metal could not begin the render pass"};
        if (!desc.label.empty()) m_impl->encoder.label = ns_string(desc.label);
        return {};
    }
}

void MetalDevice::backend_set_pipeline(uint32_t slot) {
    const auto& pipeline = m_impl->pipelines[slot];
    [m_impl->encoder setRenderPipelineState:pipeline.state];
    [m_impl->encoder setDepthStencilState:pipeline.depth];
    [m_impl->encoder setCullMode:pipeline.cull];
    [m_impl->encoder setFrontFacingWinding:pipeline.winding];
}

void MetalDevice::backend_set_vertex_buffer(uint32_t index, uint32_t slot, size_t offset) {
    [m_impl->encoder setVertexBuffer:m_impl->buffers[slot] offset:offset atIndex:index];
}

void MetalDevice::backend_set_uniform_buffer(uint32_t index, uint32_t slot, size_t offset) {
    [m_impl->encoder setVertexBuffer:m_impl->buffers[slot] offset:offset atIndex:index];
    [m_impl->encoder setFragmentBuffer:m_impl->buffers[slot] offset:offset atIndex:index];
}

void MetalDevice::backend_set_texture(uint32_t index, uint32_t slot) {
    [m_impl->encoder setFragmentTexture:m_impl->textures[slot] atIndex:index];
}

void MetalDevice::backend_set_sampler(uint32_t index, uint32_t slot) {
    [m_impl->encoder setFragmentSamplerState:m_impl->samplers[slot] atIndex:index];
}

void MetalDevice::backend_set_scissor(const ScissorRect& rect) {
    [m_impl->encoder setScissorRect:MTLScissorRect{rect.x, rect.y, rect.width, rect.height}];
}

void MetalDevice::backend_draw(uint32_t vertex_count, uint32_t first_vertex, uint32_t instance_count) {
    [m_impl->encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:first_vertex vertexCount:vertex_count
                      instanceCount:instance_count];
}

void MetalDevice::backend_draw_indexed(uint32_t slot, IndexType type, uint32_t index_count, size_t offset,
                                       uint32_t instance_count) {
    [m_impl->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:index_count
                                 indexType:type == IndexType::uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                               indexBuffer:m_impl->buffers[slot] indexBufferOffset:offset instanceCount:instance_count];
}

void MetalDevice::backend_end_pass() {
    [m_impl->encoder endEncoding];
    m_impl->encoder = nil;
}

void MetalDevice::backend_submit(uint64_t serial, bool present) {
    @autoreleasepool {
        id<MTLCommandBuffer> frame = m_impl->frame;
        m_impl->frame = nil;
        // The layer owns drawables and may discard them (e.g. on resize) while this frame runs.
        // The frame does not retain resources, so the completion handler keeps them alive.
        id<CAMetalDrawable> drawable = m_impl->drawable;
        id<MTLTexture> drawable_texture = drawable.texture;
        m_impl->drawable = nil;
        // The callbacks may run after this device is destroyed; they only touch shared completion state.
        auto state = completion();
        if (present && drawable) {
            [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
                const double presented = shown.presentedTime;
                state->record_present(serial, presented > 0.0 ? std::optional<double>(presented) : std::nullopt);
            }];
            [frame presentDrawable:drawable];
        }
        if (m_impl->encoding.pending) {
            m_impl->encoding.serial = serial;
            m_impl->pass_frames[serial % m_impl->pass_frames.size()] = std::move(m_impl->encoding);
        }
        m_impl->encoding = {};
        [frame addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
            (void)drawable;
            (void)drawable_texture;
            if (buffer.status == MTLCommandBufferStatusError)
                state->report("Frame " + std::to_string(serial) + " failed on the GPU: " + error_text(buffer.error));
            // The GPU's own timestamps for this command buffer: execution time, not CPU submit time.
            else if (buffer.GPUEndTime > buffer.GPUStartTime && buffer.GPUStartTime > 0.0)
                state->record_timing(serial, (buffer.GPUEndTime - buffer.GPUStartTime) * 1000.0, buffer.GPUStartTime);
            state->complete(serial);
        }];
        [frame commit];
        m_impl->last_submission = frame;
        const auto completed = state->completed.load(std::memory_order_acquire);
        while (!m_impl->in_flight.empty() && m_impl->in_flight.front().first <= completed) m_impl->in_flight.pop_front();
        m_impl->in_flight.emplace_back(serial, frame);
    }
}

std::string MetalDevice::backend_pass_timing_reason() const { return m_impl->pass_reason; }

void MetalDevice::backend_attach_pass_timings(GpuFrameTiming& timing) {
    @autoreleasepool {
        for (uint32_t slot = 0; slot < m_impl->pass_frames.size(); ++slot)
            if (m_impl->pass_frames[slot].pending && m_impl->pass_frames[slot].serial == timing.frame) m_impl->resolve(slot);
        const auto found = m_impl->resolved.find(timing.frame);
        if (found == m_impl->resolved.end()) return;
        const auto relative = [&](double seconds) { return (seconds - timing.started) * 1000.0; };
        const auto span = [](double start, double end) { return std::isnan(start) || std::isnan(end) ? 0.0 : std::max(0.0, end - start) * 1000.0; };
        for (const auto& pass : found->second.passes) {
            auto value = GpuPassTiming{pass.label};
            value.vertex_ms = span(pass.vertex_start, pass.vertex_end);
            value.fragment_ms = span(pass.fragment_start, pass.fragment_end);
            const auto first = std::isnan(pass.vertex_start) ? pass.fragment_start : pass.vertex_start;
            const auto last = std::isnan(pass.fragment_end) ? pass.vertex_end : pass.fragment_end;
            value.start_ms = std::isnan(first) ? 0.0 : relative(first);
            value.end_ms = std::isnan(last) ? value.start_ms : relative(last);
            timing.passes.push_back(std::move(value));
        }
        timing.untimed_passes = found->second.untimed;
        m_impl->resolved.erase(found);
    }
}

bool MetalDevice::backend_present_timing_supported() const noexcept { return m_impl->layer != nil; }

std::optional<double> MetalDevice::backend_display_refresh_rate() const noexcept {
    NSScreen* screen = m_impl->view.window.screen;
    if (!screen) return std::nullopt;
    const auto rate = screen.maximumFramesPerSecond;
    return rate > 0 ? std::optional(double(rate)) : std::nullopt;
}

std::optional<size_t> MetalDevice::backend_reported_memory() const noexcept {
    if (!m_impl->device) return std::nullopt;
    return static_cast<size_t>(m_impl->device.currentAllocatedSize);
}

bool MetalDevice::backend_wait_frame(uint64_t serial) noexcept {
    // Frames complete in commit order on one queue, so waiting for `serial` covers earlier frames.
    for (const auto& [submitted, buffer] : m_impl->in_flight)
        if (submitted == serial) {
            [buffer waitUntilCompleted];
            break;
        }
    while (!m_impl->in_flight.empty() && m_impl->in_flight.front().first <= serial) m_impl->in_flight.pop_front();
    return true;
}

void MetalDevice::backend_abandon_frame() noexcept {
    if (m_impl->encoder) {
        [m_impl->encoder endEncoding];
        m_impl->encoder = nil;
    }
    m_impl->frame = nil; // never committed, so the GPU never reads its resources
    m_impl->drawable = nil;
    m_impl->encoding = {};
}

void MetalDevice::backend_wait_idle() noexcept {
    // A queue executes command buffers in commit order, so the last one bounds all earlier work.
    [m_impl->last_submission waitUntilCompleted];
    [m_impl->last_transfer waitUntilCompleted];
    m_impl->in_flight.clear();
}

void MetalDevice::backend_release_surface(uint32_t slot) noexcept {
    if (slot < m_impl->textures.size()) m_impl->textures[slot] = nil;
}

} // namespace maya
