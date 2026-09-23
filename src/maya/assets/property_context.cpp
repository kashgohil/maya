#include "maya/assets/property_context.hpp"

namespace maya {
PropertyValidationContext asset_property_context(const AssetRegistry& registry) {
    return {[&registry](AssetId id, ReferenceKind kind) {
        const auto info = registry.info(id);
        if (!info) return ReferenceStatus::missing;
        const auto expected = kind == ReferenceKind::mesh ? AssetKind::mesh : AssetKind::material;
        return info->record.kind == expected ? ReferenceStatus::valid : ReferenceStatus::wrong_type;
    }};
}
} // namespace maya
