#include "texture_thumbnails.hpp"
#include <algorithm>

namespace maya::editor {
namespace {
struct ThumbnailConstants {
    uint32_t mode;
    float cell;
};
uint32_t mode(TextureRole role) { return role == TextureRole::normal ? 2u : role == TextureRole::data ? 1u : 0u; }
} // namespace

TextureThumbnails::TextureThumbnails(GraphicsDevice& device, UiRenderer& ui, std::string shader_source)
    : m_device(device), m_ui(ui), m_shader_source(std::move(shader_source)), m_lifetime(device.resource_lifetime()) {}

TextureThumbnails::~TextureThumbnails() { clear(); }

void TextureThumbnails::release(Entry& entry) {
    if (!m_lifetime.expired()) m_device.destroy(entry.target); // retired after frames that drew it
    entry.target = {};
    entry.version = never;
    if (entry.id) m_ui.set_texture(entry.id, {});
}

void TextureThumbnails::clear() {
    m_pending.clear();
    for (auto& [asset, entry] : m_rows) {
        release(entry);
        m_free_ids.push_back(entry.id);
    }
    m_rows.clear();
    for (auto* entry : {&m_preview, &m_placeholder_row, &m_placeholder_preview}) release(*entry);
    m_preview_asset = {};
    if (!m_lifetime.expired()) {
        m_device.destroy(m_pipeline);
        m_device.destroy(m_sampler);
    }
    m_pipeline = {};
    m_sampler = {};
    m_placeholder.reset();
}

bool TextureThumbnails::prepare(Entry& entry, uint32_t size, const std::string& label) {
    if (!entry.id) entry.id = m_ui.add_texture();
    if (entry.target.valid()) return true;
    auto created = m_device.create_texture({size, size, Format::rgba8_unorm, TextureUsage::render_target | TextureUsage::sampled | TextureUsage::readback,
                                            "thumbnail " + label});
    if (!created) return false;
    entry.target = created.handle;
    m_ui.set_texture(entry.id, entry.target);
    return true;
}

ImTextureID TextureThumbnails::request(AssetId asset, const AssetLease<TextureAsset>& lease, std::shared_ptr<const TextureAsset> owned,
                                       bool preview) {
    if (m_lifetime.expired()) { // a new device session: every earlier handle is gone
        clear();
        m_lifetime = m_device.resource_lifetime();
    }
    const auto* texture = owned ? owned.get() : lease ? &lease.value() : nullptr;
    if (!texture || !texture->valid()) return 0;
    const auto version = owned ? 0 : lease.handle().generation;
    auto* entry = &m_preview;
    if (owned) {
        entry = preview ? &m_placeholder_preview : &m_placeholder_row;
    } else if (preview) {
        if (m_preview_asset != asset) m_preview.version = never; // another texture: render again
        m_preview_asset = asset;
    } else {
        auto found = m_rows.find(asset);
        if (found == m_rows.end()) {
            // Beyond capacity, the least recently used row that was not requested this frame makes room.
            if (m_rows.size() >= row_capacity) {
                auto oldest = std::ranges::min_element(m_rows, {}, [](const auto& item) { return item.second.used; });
                if (oldest != m_rows.end() && oldest->second.used <= m_counter) {
                    std::erase_if(m_pending, [&](const Pending& item) { return item.entry == &oldest->second; });
                    release(oldest->second);
                    m_free_ids.push_back(oldest->second.id);
                    m_rows.erase(oldest);
                }
            }
            auto fresh = Entry{};
            if (!m_free_ids.empty()) {
                fresh.id = m_free_ids.back();
                m_free_ids.pop_back();
            }
            found = m_rows.emplace(asset, fresh).first;
        }
        entry = &found->second;
    }
    const auto size = preview ? preview_size : row_size;
    if (!prepare(*entry, size, texture->texture().desc().label)) return 0;
    entry->used = m_counter + 1; // this frame
    const auto pending = std::ranges::any_of(m_pending, [&](const Pending& item) { return item.entry == entry; });
    if (entry->version != version && !pending) {
        m_pending.push_back({entry, size, owned ? AssetLease<TextureAsset>{} : lease, std::move(owned)});
        entry->version = version;
    }
    return entry->id;
}

ImTextureID TextureThumbnails::current(AssetId asset, uint64_t generation, bool preview) {
    if (m_lifetime.expired() || generation == 0) return 0;
    if (preview) return m_preview_asset == asset && m_preview.version == generation && m_preview.target.valid() ? m_preview.id : 0;
    const auto found = m_rows.find(asset);
    if (found == m_rows.end() || found->second.version != generation || !found->second.target.valid()) return 0;
    found->second.used = m_counter + 1; // shown this frame: its row stays
    return found->second.id;
}

ImTextureID TextureThumbnails::placeholder(bool preview) {
    if (!m_placeholder || !m_placeholder->valid()) m_placeholder = make_placeholder_texture(m_device);
    return m_placeholder ? request({}, {}, m_placeholder, preview) : 0;
}

RhiDiagnostic TextureThumbnails::render() {
    ++m_counter;
    auto pending = std::exchange(m_pending, {});
    if (pending.empty()) return {};
    if (!m_pipeline.valid()) {
        auto desc = PipelineDesc{};
        desc.shader_source = m_shader_source;
        desc.vertex_entry = "thumbnailVertex";
        desc.fragment_entry = "thumbnailFragment";
        desc.color_formats = {Format::rgba8_unorm};
        desc.cull = CullMode::none;
        desc.label = "texture thumbnails";
        auto created = m_device.create_pipeline(desc);
        if (!created) return created.diagnostic;
        m_pipeline = created.handle;
    }
    if (!m_sampler.valid()) {
        auto created = m_device.create_sampler({Filter::linear, Filter::linear, AddressMode::clamp_to_edge, AddressMode::clamp_to_edge,
                                                "texture thumbnails", MipFilter::linear, 4});
        if (!created) return created.diagnostic;
        m_sampler = created.handle;
    }
    // On a failure, the thumbnails not yet drawn are drawn again when next requested.
    const auto retry = [&](size_t from) {
        for (auto i = from; i < pending.size(); ++i) pending[i].entry->version = never;
    };
    for (size_t index = 0; index < pending.size(); ++index) {
        auto& item = pending[index];
        const auto& texture = item.owned ? *item.owned : item.lease.value();
        if (!item.entry->target.valid() || !texture.valid()) continue;
        auto pass = RenderPassDesc{};
        pass.colors.push_back({item.entry->target, LoadAction::clear, StoreAction::store, {0.0, 0.0, 0.0, 1.0}});
        pass.label = "texture thumbnail";
        if (auto error = m_device.begin_render_pass(pass)) {
            retry(index);
            return error;
        }
        const auto constants = ThumbnailConstants{mode(texture.role()), float(item.size) / 8.0f};
        auto uploaded = m_device.upload_transient(&constants, sizeof(constants));
        auto error = uploaded ? RhiDiagnostic{} : uploaded.diagnostic;
        if (!error) error = m_device.set_pipeline(m_pipeline);
        if (!error) error = m_device.set_uniform_buffer(0, uploaded.slice);
        if (!error) error = texture.texture().bind(0);
        if (!error) error = m_device.set_sampler(0, m_sampler);
        if (!error) error = m_device.draw(3);
        if (auto ended = m_device.end_render_pass(); !error) error = ended;
        if (error) {
            retry(index);
            return error;
        }
    }
    return {};
}

size_t TextureThumbnails::gpu_bytes() const noexcept {
    const auto bytes = [](const Entry& entry, uint32_t size) { return entry.target.valid() ? size_t(size) * size * 4 : 0; }; // RGBA8
    auto total = bytes(m_preview, preview_size) + bytes(m_placeholder_row, row_size) + bytes(m_placeholder_preview, preview_size);
    for (const auto& [asset, entry] : m_rows) total += bytes(entry, row_size);
    return total;
}

} // namespace maya::editor
