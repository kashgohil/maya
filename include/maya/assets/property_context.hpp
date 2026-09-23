#pragma once
#include "maya/assets/registry.hpp"
#include "maya/properties/schema.hpp"

namespace maya {
/// Borrows the registry for synchronous validation. Registry must outlive the context.
/// Checks catalog identity/type only: no file access, load, or residency requirement.
PropertyValidationContext asset_property_context(const AssetRegistry& registry);
} // namespace maya
