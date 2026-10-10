#pragma once
#include "maya/assets/generator.hpp"
#include <filesystem>
#include <iosfwd>
#include <string>

namespace maya {
/// Generator files (#1068, docs/procedural.md#generator-files): `maya-generator 1`, then `algorithm <n>`,
/// the version of the kind's algorithm the output comes from, then one line per generator property, keyed
/// by its name (docs/properties.md#generators); a property without a line has its default.
inline constexpr uint32_t generator_format_version = 1;

struct GeneratorFileResult {
    GeneratorAsset generator;
    std::string error; // with its line number; empty on success
    explicit operator bool() const noexcept { return error.empty(); }
};
/// Reads and validates a generator file. An algorithm version this build does not generate is refused.
GeneratorFileResult read_generator_file(std::istream& input);
/// Writes every property, in schema order.
std::string write_generator_file(const GeneratorAsset& generator);
/// Replaces the file atomically (replace_file). Returns why it failed, or empty.
std::string save_generator_file(const std::filesystem::path& path, const GeneratorAsset& generator);
/// The algorithm versions this build generates for a kind: the one it writes is the newest.
uint32_t current_algorithm(GeneratorKind kind) noexcept;
} // namespace maya
