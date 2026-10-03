#include "maya/core/file_replace.hpp"
#include "maya/core/identity.hpp"
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace maya {
namespace {
std::string hex(uint64_t value) {
    char text[17];
    const auto end = std::to_chars(text, text + sizeof(text), value, 16).ptr;
    return {text, end};
}
} // namespace

std::string replace_file(const std::filesystem::path& path, std::string_view text, std::string_view what) {
    const auto unchanged = ". The existing " + std::string(what) + " file was not changed";
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    const auto exists = status.type() != std::filesystem::file_type::not_found;
    // A unique sibling keeps the final rename on one filesystem, so it replaces the file atomically.
    const auto directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const auto temporary = directory / ("." + path.filename().string() + ".tmp-" +
        hex(static_cast<uint64_t>(::getpid())) + "-" + hex(detail::next_lifetime_token()));
    const auto descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (descriptor < 0)
        return "cannot create a temporary file in '" + directory.string() + "': " + std::strerror(errno) + unchanged;
    auto descriptor_open = true;
    const auto abandon = [&](const std::string& failed, int code) {
        if (descriptor_open) ::close(descriptor);
        ::unlink(temporary.c_str());
        return failed + ": " + std::strerror(code) + unchanged;
    };
    for (size_t written = 0; written < text.size();) {
        const auto count = ::write(descriptor, text.data() + written, text.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return abandon("cannot write the " + std::string(what), count < 0 ? errno : EIO);
        written += static_cast<size_t>(count);
    }
    if (exists) ::fchmod(descriptor, static_cast<mode_t>(status.permissions()) & 07777); // best effort
    // F_FULLFSYNC asks the drive to persist data; fall back where the filesystem does not support it.
    if (::fcntl(descriptor, F_FULLFSYNC) == -1 && ::fsync(descriptor) == -1)
        return abandon("cannot flush the " + std::string(what) + " to storage", errno);
    descriptor_open = false;
    if (::close(descriptor) != 0) return abandon("cannot finish writing the " + std::string(what), errno);
    if (::rename(temporary.c_str(), path.c_str()) != 0)
        return abandon("cannot replace the " + std::string(what) + " file", errno);
    // Persist the directory entry. The new file is already in place, so failure here is not reported.
    if (const auto folder = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC); folder >= 0) {
        ::fsync(folder);
        ::close(folder);
    }
    return {};
}
} // namespace maya
