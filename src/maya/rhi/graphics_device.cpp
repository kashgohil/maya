#include "maya/rhi/graphics_device.hpp"
#include <algorithm>
#include <cmath>

namespace maya {
namespace {
std::atomic<uint64_t> g_next_session{1};

RhiDiagnostic fail(RhiError code, std::string message) { return {code, std::move(message)}; }
std::string quoted(const std::string& label) { return label.empty() ? std::string() : " '" + label + "'"; }
std::string formats_text(const std::vector<Format>& colors, Format depth) {
    auto text = std::string("[");
    for (size_t i = 0; i < colors.size(); ++i) text += (i ? ", " : "") + std::string(format_name(colors[i]));
    return text + "] + depth " + format_name(depth);
}
bool valid_bits(uint32_t value, uint32_t mask) { return value != 0 && (value & ~mask) == 0; }
} // namespace

const char* format_name(Format format) noexcept {
    switch (format) {
    case Format::undefined: return "none";
    case Format::rgba8_unorm: return "rgba8_unorm";
    case Format::rgba8_srgb: return "rgba8_srgb";
    case Format::bgra8_unorm: return "bgra8_unorm";
    case Format::bgra8_srgb: return "bgra8_srgb";
    case Format::rgba16_float: return "rgba16_float";
    case Format::depth32_float: return "depth32_float";
    }
    return "unknown";
}

void RhiCompletion::complete(uint64_t serial) noexcept {
    const auto lock = std::scoped_lock(mutex);
    auto watermark = completed.load(std::memory_order_relaxed);
    if (serial <= watermark) return;
    if (serial != watermark + 1) {
        try {
            early.push_back(serial);
        } catch (...) {
            // Dropping an early completion only delays retirement until a later watermark advance.
        }
        return;
    }
    for (watermark = serial;;) {
        const auto next = std::ranges::find(early, watermark + 1);
        if (next == early.end()) break;
        early.erase(next);
        ++watermark;
    }
    completed.store(watermark, std::memory_order_release);
}
void RhiCompletion::complete_through(uint64_t serial) noexcept {
    const auto lock = std::scoped_lock(mutex);
    if (serial <= completed.load(std::memory_order_relaxed)) return;
    std::erase_if(early, [serial](uint64_t value) { return value <= serial; });
    completed.store(serial, std::memory_order_release);
}
void RhiCompletion::report(std::string message) noexcept {
    try {
        const auto lock = std::scoped_lock(mutex);
        errors.push_back({RhiError::gpu_failure, std::move(message)});
    } catch (...) {
        // Error reporting must not throw from a completion callback.
    }
}

bool GraphicsDevice::initialize(void* native_window_handle) {
    shutdown();
    m_completion = std::make_shared<RhiCompletion>();
    m_limits = {};
    m_surface_format = Format::undefined;
    m_session = g_next_session.fetch_add(1);
    m_resource_lifetime = std::make_shared<const GraphicsResourceLifetime>();
    try {
        if (backend_initialize(native_window_handle, m_limits, m_surface_format)) return true;
    } catch (...) {
        shutdown();
        throw;
    }
    shutdown();
    return false;
}

void GraphicsDevice::shutdown() noexcept {
    if (m_session == 0) return;
    m_resource_lifetime.reset();
    try {
        if (m_state == State::pass) backend_end_pass();
    } catch (...) {
        // Abandoning the frame below discards the encoder either way.
    }
    if (m_state != State::idle) backend_abandon_frame();
    if (m_surface) {
        backend_release_surface(m_surface->texture.slot);
        free_slot(m_textures, m_surface->texture.slot);
        m_surface.reset();
    }
    m_state = State::idle;
    backend_wait_idle();
    for (const auto& retirement : m_retirements) backend_release(retirement.kind, retirement.slot);
    m_retirements.clear();
    const auto release_live = [this](auto& table, ResourceKind kind) {
        for (uint32_t slot = 0; slot < table.slots.size(); ++slot)
            if (table.slots[slot].live) backend_release(kind, slot);
        table = {};
    };
    release_live(m_buffers, ResourceKind::buffer);
    release_live(m_textures, ResourceKind::texture);
    release_live(m_samplers, ResourceKind::sampler);
    release_live(m_pipelines, ResourceKind::pipeline);
    backend_shutdown();
    m_session = 0;
    m_submitted = 0;
    m_surface_format = Format::undefined;
    m_pass_colors.clear();
    m_pass_attachments.clear();
    m_pass_depth = Format::undefined;
    m_pipeline_set = false;
}

void GraphicsDevice::resize(uint32_t width, uint32_t height) {
    if (m_session == 0 || width == 0 || height == 0) return;
    backend_resize(width, height);
}

RhiStats GraphicsDevice::stats() const noexcept {
    auto result = RhiStats{};
    result.buffers = m_buffers.live;
    result.textures = m_textures.live;
    result.samplers = m_samplers.live;
    result.pipelines = m_pipelines.live;
    result.pending_retirements = m_retirements.size();
    result.submitted_frames = m_submitted;
    result.completed_frames = std::min(m_completion->completed.load(std::memory_order_acquire), m_submitted);
    return result;
}

template<class Desc> uint32_t GraphicsDevice::allocate(Table<Desc>& table) {
    if (!table.free.empty()) {
        const auto slot = table.free.back();
        table.free.pop_back();
        return slot;
    }
    table.slots.emplace_back();
    return static_cast<uint32_t>(table.slots.size() - 1);
}

template<class Desc> void GraphicsDevice::free_slot(Table<Desc>& table, uint32_t slot) noexcept {
    auto& entry = table.slots[slot];
    entry.live = false;
    entry.desc = {};
    // An exhausted generation retires the slot instead of wrapping into an old handle's identity.
    if (entry.generation == std::numeric_limits<uint32_t>::max()) return;
    ++entry.generation;
    try {
        table.free.push_back(slot);
    } catch (...) {
        // Losing a free-list entry only leaks a reusable slot index.
    }
}

template<class Desc, class Tag>
const Desc* GraphicsDevice::lookup(const Table<Desc>& table, RhiHandle<Tag> handle, const char* kind,
                                   RhiDiagnostic* diagnostic) const {
    const auto report = [&](std::string message) -> const Desc* {
        if (diagnostic) *diagnostic = fail(RhiError::stale_handle, std::move(message));
        return nullptr;
    };
    if (!handle.valid()) return report(std::string("Null ") + kind + " handle");
    if (handle.session != m_session)
        return report(std::string(kind) + " handle belongs to another device session");
    if (handle.slot >= table.slots.size() || !table.slots[handle.slot].live ||
        table.slots[handle.slot].generation != handle.generation)
        return report(std::string(kind) + " handle was destroyed or is not from this device");
    return &table.slots[handle.slot].desc;
}

template<class Desc, class Tag>
bool GraphicsDevice::retire(Table<Desc>& table, ResourceKind kind, RhiHandle<Tag> handle) noexcept {
    if (!lookup(table, handle, "", nullptr)) return false;
    if (kind == ResourceKind::texture && m_surface && m_surface->texture == TextureHandle{handle.session, handle.slot, handle.generation})
        return false;
    // Revoke the handle immediately; the slot is reused only after native release.
    // Generation 0 marks an exhausted slot that is never reused.
    auto& entry = table.slots[handle.slot];
    entry.live = false;
    entry.generation = entry.generation == std::numeric_limits<uint32_t>::max() ? 0 : entry.generation + 1;
    --table.live;
    // A resource destroyed while a frame is encoding may be used by that frame.
    const auto serial = m_state == State::idle ? m_submitted : m_submitted + 1;
    try {
        m_retirements.push_back({kind, handle.slot, serial});
    } catch (...) {
        // Without room to defer: outside a frame, wait for the GPU and release now. Inside a frame the
        // native object stays owned by the backend until shutdown.
        if (m_state == State::idle) {
            backend_wait_idle();
            m_completion->complete_through(m_submitted);
            release_slot(kind, handle.slot);
        }
        return true;
    }
    if (m_state == State::idle) collect_retired();
    return true;
}

void GraphicsDevice::release_slot(ResourceKind kind, uint32_t slot) noexcept {
    backend_release(kind, slot);
    const auto recycle = [slot](auto& table) {
        auto& entry = table.slots[slot];
        entry.desc = {};
        // retire() already advanced the generation; 0 marks a permanently retired slot.
        if (entry.generation == 0) return;
        try {
            table.free.push_back(slot);
        } catch (...) {}
    };
    switch (kind) {
    case ResourceKind::buffer: recycle(m_buffers); break;
    case ResourceKind::texture: recycle(m_textures); break;
    case ResourceKind::sampler: recycle(m_samplers); break;
    case ResourceKind::pipeline: recycle(m_pipelines); break;
    }
}

void GraphicsDevice::collect_retired() noexcept {
    const auto completed = m_completion->completed.load(std::memory_order_acquire);
    std::erase_if(m_retirements, [&](const Retirement& retirement) {
        if (retirement.serial > completed) return false;
        release_slot(retirement.kind, retirement.slot);
        return true;
    });
}

bool GraphicsDevice::destroy(BufferHandle handle) noexcept { return retire(m_buffers, ResourceKind::buffer, handle); }
bool GraphicsDevice::destroy(TextureHandle handle) noexcept { return retire(m_textures, ResourceKind::texture, handle); }
bool GraphicsDevice::destroy(SamplerHandle handle) noexcept { return retire(m_samplers, ResourceKind::sampler, handle); }
bool GraphicsDevice::destroy(PipelineHandle handle) noexcept { return retire(m_pipelines, ResourceKind::pipeline, handle); }

const BufferDesc* GraphicsDevice::describe(BufferHandle buffer) const noexcept {
    return lookup(m_buffers, buffer, "buffer", nullptr);
}
const TextureDesc* GraphicsDevice::describe(TextureHandle texture) const noexcept {
    return lookup(m_textures, texture, "texture", nullptr);
}

RhiResult<BufferHandle> GraphicsDevice::create_buffer(const BufferDesc& desc, const void* initial_data) {
    if (m_session == 0) return {{}, fail(RhiError::device_unavailable, "Device is not initialized")};
    if (desc.size == 0 || desc.size > m_limits.max_buffer_size)
        return {{}, fail(RhiError::invalid_descriptor, "Buffer" + quoted(desc.label) + " size " +
            std::to_string(desc.size) + " is outside 1.." + std::to_string(m_limits.max_buffer_size))};
    if (!valid_bits(static_cast<uint32_t>(desc.usage), 0x7))
        return {{}, fail(RhiError::invalid_descriptor, "Buffer" + quoted(desc.label) + " needs vertex, index, or uniform usage")};
    const auto slot = allocate(m_buffers);
    auto diagnostic = RhiDiagnostic{};
    try {
        diagnostic = backend_create_buffer(slot, desc, initial_data);
    } catch (...) {
        m_buffers.free.push_back(slot);
        throw;
    }
    if (diagnostic) {
        m_buffers.free.push_back(slot);
        return {{}, std::move(diagnostic)};
    }
    auto& entry = m_buffers.slots[slot];
    entry.desc = desc;
    entry.live = true;
    ++m_buffers.live;
    return {{m_session, slot, entry.generation}, {}};
}

RhiResult<TextureHandle> GraphicsDevice::create_texture(const TextureDesc& desc, const void* initial_data) {
    if (m_session == 0) return {{}, fail(RhiError::device_unavailable, "Device is not initialized")};
    const auto name = "Texture" + quoted(desc.label);
    if (desc.width == 0 || desc.height == 0 || desc.width > m_limits.max_texture_dimension ||
        desc.height > m_limits.max_texture_dimension)
        return {{}, fail(RhiError::invalid_descriptor, name + " size " + std::to_string(desc.width) + "x" +
            std::to_string(desc.height) + " is outside 1.." + std::to_string(m_limits.max_texture_dimension))};
    if (desc.format == Format::undefined || static_cast<uint8_t>(desc.format) > static_cast<uint8_t>(Format::depth32_float))
        return {{}, fail(RhiError::invalid_descriptor, name + " needs a defined format")};
    if (!valid_bits(static_cast<uint32_t>(desc.usage), 0x7))
        return {{}, fail(RhiError::invalid_descriptor, name + " needs sampled, render_target, or readback usage")};
    if (is_depth_format(desc.format) && (initial_data || has_flag(desc.usage, TextureUsage::readback)))
        return {{}, fail(RhiError::unsupported, name + ": depth textures cannot be uploaded or read back")};
    const auto slot = allocate(m_textures);
    auto diagnostic = RhiDiagnostic{};
    try {
        diagnostic = backend_create_texture(slot, desc, initial_data);
    } catch (...) {
        m_textures.free.push_back(slot);
        throw;
    }
    if (diagnostic) {
        m_textures.free.push_back(slot);
        return {{}, std::move(diagnostic)};
    }
    auto& entry = m_textures.slots[slot];
    entry.desc = desc;
    entry.live = true;
    ++m_textures.live;
    return {{m_session, slot, entry.generation}, {}};
}

RhiResult<SamplerHandle> GraphicsDevice::create_sampler(const SamplerDesc& desc) {
    if (m_session == 0) return {{}, fail(RhiError::device_unavailable, "Device is not initialized")};
    if (desc.min_filter > Filter::linear || desc.mag_filter > Filter::linear ||
        desc.address_u > AddressMode::mirror_repeat || desc.address_v > AddressMode::mirror_repeat)
        return {{}, fail(RhiError::invalid_descriptor, "Sampler" + quoted(desc.label) + " has an unknown filter or address mode")};
    const auto slot = allocate(m_samplers);
    auto diagnostic = RhiDiagnostic{};
    try {
        diagnostic = backend_create_sampler(slot, desc);
    } catch (...) {
        m_samplers.free.push_back(slot);
        throw;
    }
    if (diagnostic) {
        m_samplers.free.push_back(slot);
        return {{}, std::move(diagnostic)};
    }
    auto& entry = m_samplers.slots[slot];
    entry.desc = desc;
    entry.live = true;
    ++m_samplers.live;
    return {{m_session, slot, entry.generation}, {}};
}

RhiResult<PipelineHandle> GraphicsDevice::create_pipeline(const PipelineDesc& desc) {
    if (m_session == 0) return {{}, fail(RhiError::device_unavailable, "Device is not initialized")};
    const auto name = "Pipeline" + quoted(desc.label);
    const auto invalid = [&](const std::string& why) {
        return RhiResult<PipelineHandle>{{}, fail(RhiError::invalid_descriptor, name + ": " + why)};
    };
    if (desc.shader_source.empty() || desc.vertex_entry.empty() || desc.fragment_entry.empty())
        return invalid("shader source and both entry points are required");
    if (desc.color_formats.empty() && desc.depth_format == Format::undefined)
        return invalid("at least one color or depth attachment format is required");
    if (desc.color_formats.size() > m_limits.max_color_attachments)
        return invalid("more than " + std::to_string(m_limits.max_color_attachments) + " color attachments");
    for (const auto format : desc.color_formats)
        if (!is_color_format(format) || static_cast<uint8_t>(format) > static_cast<uint8_t>(Format::depth32_float))
            return invalid(std::string("color attachment format ") + format_name(format) + " is not a color format");
    if (desc.depth_format != Format::undefined && !is_depth_format(desc.depth_format))
        return invalid(std::string("depth format ") + format_name(desc.depth_format) + " is not a depth format");
    if ((desc.depth.test || desc.depth.write) && desc.depth_format == Format::undefined)
        return invalid("depth test/write requires a depth attachment format");
    if (desc.depth.compare > CompareFunction::always || desc.cull > CullMode::back ||
        desc.front_face > Winding::counter_clockwise)
        return invalid("unknown compare, cull, or winding value");
    const auto slot = allocate(m_pipelines);
    auto diagnostic = RhiDiagnostic{};
    try {
        diagnostic = backend_create_pipeline(slot, desc);
    } catch (...) {
        m_pipelines.free.push_back(slot);
        throw;
    }
    if (diagnostic) {
        m_pipelines.free.push_back(slot);
        return {{}, std::move(diagnostic)};
    }
    auto& entry = m_pipelines.slots[slot];
    entry.desc = desc;
    entry.live = true;
    ++m_pipelines.live;
    return {{m_session, slot, entry.generation}, {}};
}

RhiDiagnostic GraphicsDevice::write_buffer(BufferHandle buffer, size_t offset, const void* data, size_t size) {
    auto diagnostic = RhiDiagnostic{};
    const auto desc = lookup(m_buffers, buffer, "Buffer", &diagnostic);
    if (!desc) return diagnostic;
    if (size == 0) return {};
    if (!data) return fail(RhiError::invalid_usage, "write_buffer needs data for a nonzero size");
    if (offset > desc->size || size > desc->size - offset)
        return fail(RhiError::out_of_range, "Write of " + std::to_string(size) + " bytes at offset " +
            std::to_string(offset) + " exceeds buffer" + quoted(desc->label) + " size " + std::to_string(desc->size));
    backend_write_buffer(buffer.slot, offset, data, size);
    return {};
}

RhiDiagnostic GraphicsDevice::read_texture(TextureHandle texture, std::vector<std::byte>& pixels) {
    if (auto state = require_state(State::idle, "read_texture")) return state;
    auto diagnostic = RhiDiagnostic{};
    const auto desc = lookup(m_textures, texture, "Texture", &diagnostic);
    if (!desc) return diagnostic;
    if (!has_flag(desc->usage, TextureUsage::readback))
        return fail(RhiError::invalid_usage, "Texture" + quoted(desc->label) + " was not created with readback usage");
    return backend_read_texture(texture.slot, *desc, pixels);
}

RhiDiagnostic GraphicsDevice::require_state(State state, const char* operation) const {
    if (m_session == 0) return fail(RhiError::device_unavailable, std::string(operation) + ": device is not initialized");
    if (m_state == state) return {};
    static constexpr const char* names[] = {"outside a frame", "inside a frame with no open pass", "inside a render pass"};
    return fail(RhiError::wrong_state, std::string(operation) + " must be called " + names[static_cast<int>(state)] +
        ", but the device is " + names[static_cast<int>(m_state)]);
}

RhiDiagnostic GraphicsDevice::begin_frame() {
    if (auto state = require_state(State::idle, "begin_frame")) return state;
    collect_retired();
    if (auto diagnostic = backend_begin_frame()) return diagnostic;
    m_state = State::frame;
    m_pipeline_set = false;
    return {};
}

SurfaceResult GraphicsDevice::acquire_surface() {
    if (auto state = require_state(State::frame, "acquire_surface")) return {{}, std::move(state)};
    if (m_surface) return {*m_surface, {}};
    if (m_surface_format == Format::undefined)
        return {{}, fail(RhiError::unsupported, "Headless device has no presentable surface")};
    const auto slot = allocate(m_textures);
    auto acquired = BackendSurface{};
    try {
        acquired = backend_acquire_surface(slot);
    } catch (...) {
        m_textures.free.push_back(slot);
        throw;
    }
    if (acquired.diagnostic) {
        m_textures.free.push_back(slot);
        return {{}, std::move(acquired.diagnostic)};
    }
    auto& entry = m_textures.slots[slot];
    entry.desc = {acquired.width, acquired.height, m_surface_format, TextureUsage::render_target, "surface"};
    entry.live = true;
    m_surface = SurfaceTarget{{m_session, slot, entry.generation}, acquired.width, acquired.height, m_surface_format};
    return {*m_surface, {}};
}

RhiDiagnostic GraphicsDevice::validate_attachment(const TextureHandle& handle, bool depth, uint32_t& width,
                                                  uint32_t& height) const {
    auto diagnostic = RhiDiagnostic{};
    const auto desc = lookup(m_textures, handle, depth ? "Depth attachment" : "Color attachment", &diagnostic);
    if (!desc) return diagnostic;
    const auto name = "Texture" + quoted(desc->label);
    if (!has_flag(desc->usage, TextureUsage::render_target))
        return fail(RhiError::invalid_usage, name + " was not created with render_target usage");
    if (depth != is_depth_format(desc->format))
        return fail(RhiError::invalid_usage, name + " has format " + format_name(desc->format) + ", which cannot be a " +
            (depth ? "depth" : "color") + " attachment");
    if (width == 0) {
        width = desc->width;
        height = desc->height;
    } else if (width != desc->width || height != desc->height) {
        return fail(RhiError::invalid_descriptor, name + " is " + std::to_string(desc->width) + "x" +
            std::to_string(desc->height) + "; every attachment in a pass must be " + std::to_string(width) + "x" +
            std::to_string(height));
    }
    return {};
}

RhiDiagnostic GraphicsDevice::begin_render_pass(const RenderPassDesc& desc) {
    if (auto state = require_state(State::frame, "begin_render_pass")) return state;
    const auto name = "Render pass" + quoted(desc.label);
    if (desc.colors.empty() && !desc.depth)
        return fail(RhiError::invalid_descriptor, name + " has no attachments");
    if (desc.colors.size() > m_limits.max_color_attachments)
        return fail(RhiError::invalid_descriptor, name + " has more than " +
            std::to_string(m_limits.max_color_attachments) + " color attachments");
    uint32_t width = 0, height = 0;
    auto attachments = std::vector<TextureHandle>{};
    auto colors = std::vector<Format>{};
    for (const auto& color : desc.colors) {
        if (auto diagnostic = validate_attachment(color.texture, false, width, height)) return diagnostic;
        if (color.load > LoadAction::dont_care || color.store > StoreAction::dont_care)
            return fail(RhiError::invalid_descriptor, name + " has an unknown load or store action");
        if (!std::ranges::all_of(color.clear_color, [](double value) { return std::isfinite(value); }))
            return fail(RhiError::invalid_descriptor, name + " has a nonfinite clear color");
        attachments.push_back(color.texture);
        colors.push_back(describe(color.texture)->format);
    }
    auto depth_format = Format::undefined;
    if (desc.depth) {
        if (auto diagnostic = validate_attachment(desc.depth->texture, true, width, height)) return diagnostic;
        if (desc.depth->load > LoadAction::dont_care || desc.depth->store > StoreAction::dont_care)
            return fail(RhiError::invalid_descriptor, name + " has an unknown depth load or store action");
        if (!(desc.depth->clear_depth >= 0.0 && desc.depth->clear_depth <= 1.0))
            return fail(RhiError::invalid_descriptor, name + " clear depth must be within [0, 1]");
        attachments.push_back(desc.depth->texture);
        depth_format = describe(desc.depth->texture)->format;
    }
    for (size_t i = 0; i < attachments.size(); ++i)
        if (std::find(attachments.begin() + static_cast<std::ptrdiff_t>(i) + 1, attachments.end(), attachments[i]) != attachments.end())
            return fail(RhiError::invalid_usage, name + " uses the same texture for more than one attachment");
    if (auto diagnostic = backend_begin_pass(desc)) return diagnostic;
    m_pass_colors = std::move(colors);
    m_pass_depth = depth_format;
    m_pass_attachments = std::move(attachments);
    m_pipeline_set = false;
    m_state = State::pass;
    return {};
}

RhiDiagnostic GraphicsDevice::set_pipeline(PipelineHandle pipeline) {
    if (auto state = require_state(State::pass, "set_pipeline")) return state;
    auto diagnostic = RhiDiagnostic{};
    const auto desc = lookup(m_pipelines, pipeline, "Pipeline", &diagnostic);
    if (!desc) return diagnostic;
    if (desc->color_formats != m_pass_colors || desc->depth_format != m_pass_depth)
        return fail(RhiError::incompatible_pipeline, "Pipeline" + quoted(desc->label) + " targets " +
            formats_text(desc->color_formats, desc->depth_format) + ", but the pass has " +
            formats_text(m_pass_colors, m_pass_depth));
    backend_set_pipeline(pipeline.slot);
    m_pipeline_set = true;
    return {};
}

RhiDiagnostic GraphicsDevice::set_vertex_buffer(uint32_t index, BufferHandle buffer, size_t offset) {
    if (auto state = require_state(State::pass, "set_vertex_buffer")) return state;
    if (index >= m_limits.max_buffer_slots)
        return fail(RhiError::out_of_range, "Buffer index " + std::to_string(index) + " exceeds the limit of " +
            std::to_string(m_limits.max_buffer_slots));
    auto diagnostic = RhiDiagnostic{};
    const auto desc = lookup(m_buffers, buffer, "Vertex buffer", &diagnostic);
    if (!desc) return diagnostic;
    if (!has_flag(desc->usage, BufferUsage::vertex))
        return fail(RhiError::invalid_usage, "Buffer" + quoted(desc->label) + " was not created with vertex usage");
    if (offset >= desc->size)
        return fail(RhiError::out_of_range, "Vertex offset " + std::to_string(offset) + " is outside buffer" +
            quoted(desc->label) + " of size " + std::to_string(desc->size));
    if (offset % m_limits.vertex_offset_alignment)
        return fail(RhiError::misaligned, "Vertex offset " + std::to_string(offset) + " must be a multiple of " +
            std::to_string(m_limits.vertex_offset_alignment));
    backend_set_vertex_buffer(index, buffer.slot, offset);
    return {};
}

RhiDiagnostic GraphicsDevice::set_uniform_buffer(uint32_t index, BufferHandle buffer, size_t offset) {
    if (auto state = require_state(State::pass, "set_uniform_buffer")) return state;
    if (index >= m_limits.max_buffer_slots)
        return fail(RhiError::out_of_range, "Buffer index " + std::to_string(index) + " exceeds the limit of " +
            std::to_string(m_limits.max_buffer_slots));
    auto diagnostic = RhiDiagnostic{};
    const auto desc = lookup(m_buffers, buffer, "Uniform buffer", &diagnostic);
    if (!desc) return diagnostic;
    if (!has_flag(desc->usage, BufferUsage::uniform))
        return fail(RhiError::invalid_usage, "Buffer" + quoted(desc->label) + " was not created with uniform usage");
    if (offset >= desc->size)
        return fail(RhiError::out_of_range, "Uniform offset " + std::to_string(offset) + " is outside buffer" +
            quoted(desc->label) + " of size " + std::to_string(desc->size));
    if (offset % m_limits.uniform_offset_alignment)
        return fail(RhiError::misaligned, "Uniform offset " + std::to_string(offset) + " must be a multiple of " +
            std::to_string(m_limits.uniform_offset_alignment));
    backend_set_uniform_buffer(index, buffer.slot, offset);
    return {};
}

RhiDiagnostic GraphicsDevice::set_texture(uint32_t index, TextureHandle texture) {
    if (auto state = require_state(State::pass, "set_texture")) return state;
    if (index >= m_limits.max_texture_slots)
        return fail(RhiError::out_of_range, "Texture index " + std::to_string(index) + " exceeds the limit of " +
            std::to_string(m_limits.max_texture_slots));
    auto diagnostic = RhiDiagnostic{};
    const auto desc = lookup(m_textures, texture, "Texture", &diagnostic);
    if (!desc) return diagnostic;
    if (!has_flag(desc->usage, TextureUsage::sampled))
        return fail(RhiError::invalid_usage, "Texture" + quoted(desc->label) + " was not created with sampled usage");
    if (std::ranges::find(m_pass_attachments, texture) != m_pass_attachments.end())
        return fail(RhiError::invalid_usage, "Texture" + quoted(desc->label) +
            " is an attachment of the current pass and cannot also be sampled");
    backend_set_texture(index, texture.slot);
    return {};
}

RhiDiagnostic GraphicsDevice::set_sampler(uint32_t index, SamplerHandle sampler) {
    if (auto state = require_state(State::pass, "set_sampler")) return state;
    if (index >= m_limits.max_sampler_slots)
        return fail(RhiError::out_of_range, "Sampler index " + std::to_string(index) + " exceeds the limit of " +
            std::to_string(m_limits.max_sampler_slots));
    auto diagnostic = RhiDiagnostic{};
    if (!lookup(m_samplers, sampler, "Sampler", &diagnostic)) return diagnostic;
    backend_set_sampler(index, sampler.slot);
    return {};
}

RhiDiagnostic GraphicsDevice::draw(uint32_t vertex_count, uint32_t first_vertex, uint32_t instance_count) {
    if (auto state = require_state(State::pass, "draw")) return state;
    if (!m_pipeline_set) return fail(RhiError::wrong_state, "draw requires set_pipeline in the current pass");
    if (vertex_count == 0 || instance_count == 0)
        return fail(RhiError::invalid_usage, "draw needs a nonzero vertex and instance count");
    backend_draw(vertex_count, first_vertex, instance_count);
    return {};
}

RhiDiagnostic GraphicsDevice::draw_indexed(BufferHandle indices, IndexType type, uint32_t index_count,
                                           size_t offset, uint32_t instance_count) {
    if (auto state = require_state(State::pass, "draw_indexed")) return state;
    if (!m_pipeline_set) return fail(RhiError::wrong_state, "draw_indexed requires set_pipeline in the current pass");
    auto diagnostic = RhiDiagnostic{};
    const auto desc = lookup(m_buffers, indices, "Index buffer", &diagnostic);
    if (!desc) return diagnostic;
    if (!has_flag(desc->usage, BufferUsage::index))
        return fail(RhiError::invalid_usage, "Buffer" + quoted(desc->label) + " was not created with index usage");
    if (type > IndexType::uint32) return fail(RhiError::invalid_usage, "Unknown index type");
    if (index_count == 0 || instance_count == 0)
        return fail(RhiError::invalid_usage, "draw_indexed needs a nonzero index and instance count");
    if (offset % 4)
        return fail(RhiError::misaligned, "Index buffer offset " + std::to_string(offset) + " must be a multiple of 4");
    const auto bytes = static_cast<uint64_t>(index_count) * index_size(type);
    if (offset > desc->size || bytes > desc->size - offset)
        return fail(RhiError::out_of_range, std::to_string(index_count) + " indices at offset " + std::to_string(offset) +
            " exceed index buffer" + quoted(desc->label) + " of size " + std::to_string(desc->size));
    backend_draw_indexed(indices.slot, type, index_count, offset, instance_count);
    return {};
}

RhiDiagnostic GraphicsDevice::end_render_pass() {
    if (auto state = require_state(State::pass, "end_render_pass")) return state;
    backend_end_pass();
    m_state = State::frame;
    m_pass_colors.clear();
    m_pass_attachments.clear();
    m_pass_depth = Format::undefined;
    m_pipeline_set = false;
    return {};
}

RhiDiagnostic GraphicsDevice::end_frame() {
    if (m_session == 0) return fail(RhiError::device_unavailable, "end_frame: device is not initialized");
    if (m_state == State::idle) return fail(RhiError::wrong_state, "end_frame called without begin_frame");
    auto result = RhiDiagnostic{};
    if (m_state == State::pass) {
        end_render_pass();
        result = fail(RhiError::wrong_state, "end_frame closed a render pass that was still open");
    }
    const auto surface = std::exchange(m_surface, std::nullopt);
    m_state = State::idle;
    const auto release_surface = [&] {
        if (!surface) return;
        backend_release_surface(surface->texture.slot);
        free_slot(m_textures, surface->texture.slot);
    };
    try {
        backend_submit(++m_submitted, surface.has_value());
    } catch (...) {
        release_surface();
        throw;
    }
    release_surface();
    collect_retired();
    return result;
}

void GraphicsDevice::wait_idle() noexcept {
    if (m_session == 0) return;
    backend_wait_idle();
    m_completion->complete_through(m_submitted);
    if (m_state == State::idle) collect_retired();
}

std::vector<RhiDiagnostic> GraphicsDevice::take_gpu_errors() {
    const auto lock = std::scoped_lock(m_completion->mutex);
    return std::exchange(m_completion->errors, {});
}

} // namespace maya
