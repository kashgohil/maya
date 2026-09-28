#pragma once
// Minimal PNG for test reference images: 8-bit RGB, no interlacing, one IDAT, filter 0 on every row.
// It reads only what it writes; that is all the references need. Uses the system zlib.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>
#include <zlib.h>

namespace maya::test {

struct RgbImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgb; // width × height × 3, rows top to bottom
};

namespace png_detail {
inline void put32(std::string& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out += char((value >> shift) & 0xff);
}
inline uint32_t get32(const std::string& in, size_t at) {
    return uint32_t(uint8_t(in[at])) << 24 | uint32_t(uint8_t(in[at + 1])) << 16 | uint32_t(uint8_t(in[at + 2])) << 8 |
           uint32_t(uint8_t(in[at + 3]));
}
inline void chunk(std::string& out, const char* type, const std::string& data) {
    put32(out, uint32_t(data.size()));
    const auto start = out.size();
    out.append(type, 4);
    out += data;
    put32(out, uint32_t(crc32(0, reinterpret_cast<const Bytef*>(out.data() + start), uInt(out.size() - start))));
}
inline const std::string signature = std::string("\x89PNG\r\n\x1a\n", 8);
} // namespace png_detail

inline bool write_png(const std::filesystem::path& path, const RgbImage& image) {
    using namespace png_detail;
    auto raw = std::string{};
    raw.reserve(size_t(image.height) * (image.width * 3 + 1));
    for (uint32_t y = 0; y < image.height; ++y) {
        raw += '\0';
        raw.append(reinterpret_cast<const char*>(image.rgb.data() + size_t(y) * image.width * 3), image.width * 3);
    }
    auto compressed = std::string(compressBound(uLong(raw.size())), '\0');
    auto size = uLongf(compressed.size());
    if (compress2(reinterpret_cast<Bytef*>(compressed.data()), &size, reinterpret_cast<const Bytef*>(raw.data()),
                  uLong(raw.size()), 9) != Z_OK)
        return false;
    compressed.resize(size);
    auto header = std::string{};
    put32(header, image.width);
    put32(header, image.height);
    header += std::string("\x08\x02\x00\x00\x00", 5); // 8-bit, RGB, deflate, filter 0, no interlace
    auto out = signature;
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", compressed);
    chunk(out, "IEND", {});
    auto file = std::ofstream(path, std::ios::binary);
    file << out;
    return bool(file);
}

inline std::optional<RgbImage> read_png(const std::filesystem::path& path) {
    using namespace png_detail;
    auto file = std::ifstream(path, std::ios::binary);
    const auto in = std::string(std::istreambuf_iterator<char>(file), {});
    if (in.size() < 33 || in.compare(0, 8, signature) != 0) return std::nullopt;
    auto image = RgbImage{};
    auto compressed = std::string{};
    for (size_t at = 8; at + 12 <= in.size();) {
        const auto length = get32(in, at);
        const auto type = in.substr(at + 4, 4);
        if (at + 12 + length > in.size()) return std::nullopt;
        const auto data = in.substr(at + 8, length);
        if (type == "IHDR") {
            if (length != 13 || data.substr(8, 5) != std::string("\x08\x02\x00\x00\x00", 5)) return std::nullopt;
            image.width = get32(data, 0);
            image.height = get32(data, 4);
        } else if (type == "IDAT") {
            compressed += data;
        }
        at += 12 + length;
    }
    const auto row = size_t(image.width) * 3 + 1;
    auto raw = std::string(row * image.height, '\0');
    auto size = uLongf(raw.size());
    if (image.width == 0 ||
        uncompress(reinterpret_cast<Bytef*>(raw.data()), &size, reinterpret_cast<const Bytef*>(compressed.data()),
                   uLong(compressed.size())) != Z_OK ||
        size != raw.size())
        return std::nullopt;
    image.rgb.resize(size_t(image.width) * image.height * 3);
    for (uint32_t y = 0; y < image.height; ++y) {
        if (raw[y * row] != '\0') return std::nullopt; // only filter 0 is written
        std::memcpy(image.rgb.data() + size_t(y) * image.width * 3, raw.data() + y * row + 1, image.width * 3);
    }
    return image;
}

} // namespace maya::test
