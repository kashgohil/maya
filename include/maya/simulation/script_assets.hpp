#pragma once

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

} // namespace maya
