#pragma once
#include "maya/assets/material.hpp"
#include <filesystem>
#include <iosfwd>
#include <string>

namespace maya {
/// Material files (docs/assets.md#materials). Version 2 has one line per material property, keyed by
/// its name (docs/properties.md#materials); a property without a line has its default. Version 1
/// (base color, metallic, and roughness, in that order) still loads; saving writes version 2.
inline constexpr uint32_t material_format_version = 2;

struct MaterialFileResult {
    MaterialAsset material;
    uint32_t version = 0; // the version read; 0 on failure
    std::string error; // with its line number; empty on success
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Reads and validates a material file; texture references are checked for form only, not against a catalog.
MaterialFileResult read_material_file(std::istream& input);
/// Writes every property, in schema order, at material_format_version.
std::string write_material_file(const MaterialAsset& material);
/// Replaces the file atomically (replace_file) with write_material_file's text. Returns why it failed,
/// or empty.
std::string save_material_file(const std::filesystem::path& path, const MaterialAsset& material);
} // namespace maya
