#include "maya/rhi/metal/metal_device.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
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
    id<MTLRenderCommandEncoder> encoder = nil;
    id<CAMetalDrawable> drawable = nil;
    std::vector<id<MTLBuffer>> buffers;
    std::vector<id<MTLTexture>> textures;
    std::vector<id<MTLSamplerState>> samplers;
    std::vector<MetalPipeline> pipelines;
    // Keeps the most recent transfer ordered before later frames and alive for wait_idle.
    id<MTLCommandBuffer> last_transfer = nil;
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
        m_impl->last_transfer = nil;
        if (m_impl->layer && m_impl->view.layer == m_impl->layer) m_impl->view.layer = nil;
        m_impl->view = nil;
        m_impl->layer = nil;
        m_impl->queue = nil;
        m_impl->device = nil;
    }
}

void MetalDevice::backend_resize(uint32_t width, uint32_t height) {
    if (m_impl->layer) m_impl->layer.drawableSize = CGSizeMake(width, height);
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
        auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format(desc.format)
                                                                              width:desc.width
                                                                             height:desc.height
                                                                          mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageUnknown;
        if (has_flag(desc.usage, TextureUsage::sampled)) descriptor.usage |= MTLTextureUsageShaderRead;
        if (has_flag(desc.usage, TextureUsage::render_target)) descriptor.usage |= MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [m_impl->device newTextureWithDescriptor:descriptor];
        if (!texture) return {RhiError::out_of_memory, "Metal could not allocate a " + std::to_string(desc.width) + "x" +
            std::to_string(desc.height) + " " + format_name(desc.format) + " texture"};
        if (!desc.label.empty()) texture.label = ns_string(desc.label);
        if (data) {
            // Private textures are filled through a staging copy ordered before later frames.
            const auto row = size_t{desc.width} * bytes_per_pixel(desc.format);
            id<MTLBuffer> staging = [m_impl->device newBufferWithBytes:data length:row * desc.height
                                                               options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> upload = [m_impl->queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [upload blitCommandEncoder];
            if (!staging || !upload || !blit) return {RhiError::out_of_memory, "Metal could not stage texture data"};
            [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * desc.height
                      sourceSize:MTLSizeMake(desc.width, desc.height, 1) toTexture:texture destinationSlice:0
                destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
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
        for (size_t i = 0; i < desc.color_formats.size(); ++i)
            descriptor.colorAttachments[i].pixelFormat = pixel_format(desc.color_formats[i]);
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
        if (present && drawable) [frame presentDrawable:drawable];
        // The callback may run after this device is destroyed; it only touches shared completion state.
        auto state = completion();
        [frame addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
            (void)drawable;
            (void)drawable_texture;
            if (buffer.status == MTLCommandBufferStatusError)
                state->report("Frame " + std::to_string(serial) + " failed on the GPU: " + error_text(buffer.error));
            state->complete(serial);
        }];
        [frame commit];
        m_impl->last_submission = frame;
    }
}

void MetalDevice::backend_abandon_frame() noexcept {
    if (m_impl->encoder) {
        [m_impl->encoder endEncoding];
        m_impl->encoder = nil;
    }
    m_impl->frame = nil; // never committed, so the GPU never reads its resources
    m_impl->drawable = nil;
}

void MetalDevice::backend_wait_idle() noexcept {
    // A queue executes command buffers in commit order, so the last one bounds all earlier work.
    [m_impl->last_submission waitUntilCompleted];
    [m_impl->last_transfer waitUntilCompleted];
}

void MetalDevice::backend_release_surface(uint32_t slot) noexcept {
    if (slot < m_impl->textures.size()) m_impl->textures[slot] = nil;
}

} // namespace maya
