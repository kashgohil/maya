#include "maya/assets/property_context.hpp"

namespace maya {
AssetKind reference_asset_kind(ReferenceKind kind) noexcept {
    switch (kind) {
    case ReferenceKind::mesh: return AssetKind::mesh;
    case ReferenceKind::material: return AssetKind::material;
    case ReferenceKind::script: return AssetKind::script;
    case ReferenceKind::texture: return AssetKind::texture;
    case ReferenceKind::environment: return AssetKind::environment;
    case ReferenceKind::skin: return AssetKind::skin;
    case ReferenceKind::animation: return AssetKind::animation;
    }
    return AssetKind::mesh;
}

PropertyValidationContext asset_property_context(const AssetRegistry& registry) {
    return {[&registry](AssetId id, ReferenceKind kind) {
        const auto info = registry.info(id);
        if (!info) return ReferenceStatus::missing;
        return info->record.kind == reference_asset_kind(kind) ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
    }};
}
} // namespace maya
