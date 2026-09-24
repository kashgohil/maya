#pragma once

#include "maya/scene/scene_io.hpp"
#include <map>
#include <tuple>

namespace maya::detail {
/// Source locations for diagnostics about a document parsed from text.
struct SceneLines {
    std::vector<size_t> entity; // indexed like SceneDocument::entities
    std::vector<size_t> parent; // 0 when the entity has no parent line
    std::map<std::tuple<size_t, ComponentId, PropertyId>, size_t> fields; // property 0 = header
    bool components_validated = false; // the parser already ran component validation
};

/// Appends until the cap, then records a single truncation note.
class SceneReport {
public:
    explicit SceneReport(SceneDiagnostics& output) : m_output(output) {}
    void add(SceneError code, std::string message, size_t line = 0, EntityId entity = {});
private:
    SceneDiagnostics& m_output;
};

bool valid_utf8(std::string_view text);
std::string id_text(uint64_t high, uint64_t low);
template<class Tag> std::string id_text(PersistentId<Tag> id) { return id_text(id.high, id.low); }
std::string entity_location(const SceneEntity& entity);
/// Reports a failed property/component validation. `value` is the rejected property value, if known.
void report_property_error(SceneReport& report, const PropertyResult& result, ComponentId component,
                           const std::optional<PropertyValue>& value, const std::string& location,
                           size_t line, EntityId entity);
SceneDiagnostics validate_scene(SceneDocument& document, const PropertyValidationContext& context,
                                const SceneLines* lines);
} // namespace maya::detail
