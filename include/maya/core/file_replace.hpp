#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace maya {
/// Writes `text` to a private temporary file beside `path`, flushes it to storage, then renames it over
/// `path`, so a reader sees the previous file or the new one, never part of one, and any failure leaves
/// the previous file untouched. An existing file's permissions are kept. Returns empty on success, or
/// why it failed, naming the content as `what` (e.g. "scene": "cannot write the scene: ... The existing
/// scene file was not changed").
std::string replace_file(const std::filesystem::path& path, std::string_view text, std::string_view what);
} // namespace maya
