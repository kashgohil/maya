#pragma once

#include "maya/assets/project.hpp"
#include "maya/assets/registry.hpp"
#include "maya/simulation/scripting.hpp"

namespace maya {

/// Script sources from a project's asset registry, named by their catalog paths. The registry must
/// outlive the play session.
inline ScriptSources registry_script_sources(AssetRegistry& registry) {
    return [&registry](AssetId id) -> ScriptSourceResult {
        const auto loaded = registry.acquire(AssetRef<ScriptAsset>{id});
        if (!loaded) return {std::nullopt, loaded.diagnostic.message};
        const auto info = registry.info(id);
        return {ScriptSource{info ? info->record.path.generic_string() : std::string("script"), loaded.lease.value().source}, {}};
    };
}

/// Script settings for a project's play sessions: the limits its file sets, and the defaults otherwise.
inline ScriptSettings project_script_settings(const ProjectSettings& project) {
    auto settings = ScriptSettings{};
    if (project.script_work) settings.limits.work_per_call = *project.script_work;
    if (project.script_memory) settings.limits.memory_bytes = size_t{*project.script_memory} << 20;
    return settings;
}

} // namespace maya
