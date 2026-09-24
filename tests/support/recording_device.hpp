#pragma once

#include "maya/rhi/null_device.hpp"
#include <catch2/catch_test_macros.hpp>

namespace maya::test {

/// Null device that records what reaches the backend. It starts inside an open frame and render
/// pass with a pipeline set, so mesh and texture helpers can encode immediately.
class RecordingDevice : public NullGraphicsDevice {
public:
    explicit RecordingDevice(NullDeviceOptions options = {}) : NullGraphicsDevice(options) {
        REQUIRE(initialize(nullptr));
        m_target = create_texture({4, 4, Format::rgba8_unorm, TextureUsage::render_target, "test target"}).handle;
        auto pipeline = PipelineDesc{};
        pipeline.shader_source = "test source";
        pipeline.color_formats = {Format::rgba8_unorm};
        m_pipeline = create_pipeline(pipeline).handle;
        REQUIRE(m_target.valid());
        REQUIRE(m_pipeline.valid());
        last_texture_data = nullptr; // record only what tests create
        last_texture_width = last_texture_height = 0;
        open_pass();
    }
    ~RecordingDevice() override { shutdown(); }

    void open_pass() {
        REQUIRE_FALSE(begin_frame());
        auto pass = RenderPassDesc{};
        pass.colors.push_back({m_target});
        REQUIRE_FALSE(begin_render_pass(pass));
        REQUIRE_FALSE(set_pipeline(m_pipeline));
    }
    void close_frame() {
        REQUIRE_FALSE(end_render_pass());
        REQUIRE_FALSE(end_frame());
    }

    size_t last_vertex_buffer_size = 0;
    size_t last_index_buffer_size = 0;
    uint32_t last_draw_count = 0;
    size_t draws = 0;
    const void* last_texture_data = nullptr;
    uint32_t last_texture_width = 0;
    uint32_t last_texture_height = 0;
    uint32_t last_bind_slot = 0;

protected:
    RhiDiagnostic backend_create_buffer(uint32_t slot, const BufferDesc& desc, const void* data) override {
        if (has_flag(desc.usage, BufferUsage::vertex)) last_vertex_buffer_size = desc.size;
        if (has_flag(desc.usage, BufferUsage::index)) last_index_buffer_size = desc.size;
        return NullGraphicsDevice::backend_create_buffer(slot, desc, data);
    }
    RhiDiagnostic backend_create_texture(uint32_t slot, const TextureDesc& desc, const void* data) override {
        last_texture_data = data;
        last_texture_width = desc.width;
        last_texture_height = desc.height;
        return NullGraphicsDevice::backend_create_texture(slot, desc, data);
    }
    void backend_set_texture(uint32_t index, uint32_t slot) override {
        last_bind_slot = index;
        NullGraphicsDevice::backend_set_texture(index, slot);
    }
    void backend_draw_indexed(uint32_t slot, IndexType type, uint32_t count, size_t offset, uint32_t instances) override {
        last_draw_count = count;
        ++draws;
        NullGraphicsDevice::backend_draw_indexed(slot, type, count, offset, instances);
    }

private:
    TextureHandle m_target;
    PipelineHandle m_pipeline;
};

} // namespace maya::test
