#pragma once

#include "maya/assets/registry.hpp"
#include "ui_renderer.hpp"
#include <memory>
#include <unordered_map>
#include <vector>

namespace maya::editor {

/// Small images of texture assets for the Assets panel, rendered on the GPU from each texture's own
/// mip levels and shown in the display's encoding (color re-encoded to sRGB over a checkerboard, data
/// as stored, normal maps with z rebuilt). Rows keep a thumbnail per texture version, least recently
/// used first out beyond `row_capacity`; tooltips share one larger preview. Main thread only.
class TextureThumbnails {
public:
    static constexpr uint32_t row_size = 40; // framebuffer pixels
    static constexpr uint32_t preview_size = 192;
    static constexpr size_t row_capacity = 256;

    /// `shader_source` is editor_ui.metal (thumbnailVertex and thumbnailFragment).
    TextureThumbnails(GraphicsDevice& device, UiRenderer& ui, std::string shader_source);
    ~TextureThumbnails();
    TextureThumbnails(const TextureThumbnails&) = delete;
    TextureThumbnails& operator=(const TextureThumbnails&) = delete;

    /// The UI texture showing this version of `asset`, rendered before this frame's UI is drawn.
    ImTextureID row(AssetId asset, const AssetLease<TextureAsset>& texture) { return request(asset, texture, {}, false); }
    ImTextureID preview(AssetId asset, const AssetLease<TextureAsset>& texture) { return request(asset, texture, {}, true); }
    /// The thumbnail already made of this version of `asset`, or 0: what a row shows without holding the
    /// texture, or loading it again once it is released (#1063).
    ImTextureID current(AssetId asset, uint64_t generation, bool preview);
    /// The placeholder's thumbnail, for a texture that is missing or failed.
    ImTextureID placeholder(bool preview);
    /// Inside a frame with no pass open: renders the thumbnails requested since the last call.
    RhiDiagnostic render();
    /// Forgets every thumbnail, e.g. when the project's registry is replaced.
    void clear();
    size_t cached() const noexcept { return m_rows.size(); }
    /// Tracked bytes of the thumbnails' targets (#1063: the editor's own GPU memory).
    size_t gpu_bytes() const noexcept;
    /// The target holding a texture's row thumbnail (readable, for tests), or none.
    TextureHandle row_target(AssetId asset) const {
        const auto found = m_rows.find(asset);
        return found == m_rows.end() ? TextureHandle{} : found->second.target;
    }

private:
    static constexpr uint64_t never = ~uint64_t{0}; // not rendered yet
    struct Entry {
        ImTextureID id = 0;
        TextureHandle target;
        uint64_t version = never; // asset generation rendered; 0 for the placeholder
        uint64_t used = 0; // request counter, for eviction
    };
    struct Pending {
        Entry* entry = nullptr;
        uint32_t size = 0;
        AssetLease<TextureAsset> lease; // keeps the texture alive until it is encoded
        std::shared_ptr<const TextureAsset> owned;
    };
    ImTextureID request(AssetId asset, const AssetLease<TextureAsset>& lease, std::shared_ptr<const TextureAsset> owned, bool preview);
    bool prepare(Entry& entry, uint32_t size, const std::string& label);
    void release(Entry& entry);

    GraphicsDevice& m_device;
    UiRenderer& m_ui;
    std::string m_shader_source;
    std::weak_ptr<const GraphicsResourceLifetime> m_lifetime;
    PipelineHandle m_pipeline;
    SamplerHandle m_sampler;
    std::shared_ptr<const TextureAsset> m_placeholder;
    std::unordered_map<AssetId, Entry, PersistentIdHash> m_rows;
    Entry m_preview;
    AssetId m_preview_asset{};
    Entry m_placeholder_row, m_placeholder_preview;
    std::vector<ImTextureID> m_free_ids; // from evicted rows, reused
    std::vector<Pending> m_pending;
    uint64_t m_counter = 0;
};

} // namespace maya::editor
