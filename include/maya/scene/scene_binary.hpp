#pragma once

#include "maya/scene/scene_io.hpp"
#include <string>
#include <string_view>

namespace maya {

/// The packed binary form of a scene document (#1064, docs/scene.md#cooked-cells): what a world's cell
/// scenes are cooked to, so loading a cell costs decoding, not parsing. It holds every property of every
/// component at this build's schema versions, and a fingerprint of those schemas: a cook made by a build
/// with other schemas is refused, and cooked again from its text. It is a cache format, never authored.
std::string encode_scene_binary(const SceneDocument& document);
/// Decodes and validates, as read_scene does for text. A malformed buffer or another build's schemas are
/// diagnostics, never a crash.
SceneDocumentResult decode_scene_binary(std::string_view bytes, const PropertyValidationContext& context);
/// The fingerprint of this build's component schemas (names, versions, property IDs and types).
uint64_t scene_schema_fingerprint();

} // namespace maya
