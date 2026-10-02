// KTX 2.0 (https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html), the subset in
// docs/assets.md#ktx2: one 2D image and its mip levels, no supercompression.
#include "maya/assets/texture_data.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace maya {
namespace {
constexpr auto identifier = std::array<uint8_t, 12>{0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
constexpr size_t header_bytes = 80; // identifier, nine words, and the index
constexpr size_t level_entry_bytes = 24;

// Vulkan format numbers (VkFormat) and Khronos Data Format values.
struct FormatCode {
    Format format;
    uint32_t vk;
};
constexpr auto format_codes = std::array{
    FormatCode{Format::rgba8_unorm, 37}, FormatCode{Format::rgba8_srgb, 43},
    FormatCode{Format::astc_4x4_unorm, 157}, FormatCode{Format::astc_4x4_srgb, 158},
    FormatCode{Format::astc_6x6_unorm, 165}, FormatCode{Format::astc_6x6_srgb, 166}};
constexpr uint32_t model_rgbsda = 1, model_astc = 162;
constexpr uint32_t primaries_bt709 = 1;
constexpr uint32_t transfer_linear = 1, transfer_srgb = 2;
constexpr uint32_t qualifier_linear = 0x10; // a sample stored linearly in an otherwise sRGB format
constexpr uint32_t channel_alpha = 15;

uint32_t vk_format(Format format) {
    for (const auto& code : format_codes)
        if (code.format == format) return code.vk;
    return 0;
}
/// Level data offsets are multiples of the texel block size and of 4 (lcm, as the spec requires).
size_t level_alignment(Format format) { return std::lcm(std::max<size_t>(block_bytes(format), 1), size_t{4}); }
size_t align(size_t value, size_t alignment) { return (value + alignment - 1) / alignment * alignment; }

void put32(std::vector<std::byte>& out, size_t offset, uint32_t value) { std::memcpy(out.data() + offset, &value, 4); }
void put64(std::vector<std::byte>& out, size_t offset, uint64_t value) { std::memcpy(out.data() + offset, &value, 8); }
uint32_t get32(std::span<const std::byte> in, size_t offset) {
    auto value = uint32_t{};
    std::memcpy(&value, in.data() + offset, 4);
    return value;
}
uint64_t get64(std::span<const std::byte> in, size_t offset) {
    auto value = uint64_t{};
    std::memcpy(&value, in.data() + offset, 8);
    return value;
}

/// The basic data format descriptor for a format, as a list of words (dfdTotalSize first).
std::vector<uint32_t> data_format_descriptor(Format format) {
    const auto srgb = is_srgb_format(format);
    const auto transfer = srgb ? transfer_srgb : transfer_linear;
    auto words = std::vector<uint32_t>{};
    const auto block = [&](uint32_t model, uint32_t dimensions, uint32_t bytes, uint32_t samples) {
        const auto block_size = 24 + 16 * samples;
        words = {4 + block_size, 0, 2u | (block_size << 16), model | (primaries_bt709 << 8) | (transfer << 16), dimensions, bytes, 0};
    };
    if (is_compressed_format(format)) {
        const auto extent = block_extent(format) - 1;
        block(model_astc, extent | (extent << 8), 16, 1);
        words.insert(words.end(), {0u | (127u << 16), 0u, 0u, 0xFFFFFFFFu}); // 128 bits of ASTC data
    } else {
        block(model_rgbsda, 0, 4, 4);
        for (uint32_t channel = 0; channel < 4; ++channel) {
            const auto type = channel == 3 ? channel_alpha | (srgb ? qualifier_linear : 0) : channel;
            words.insert(words.end(), {channel * 8 | (7u << 16) | (type << 24), 0u, 0u, 255u});
        }
    }
    return words;
}
} // namespace

bool ktx2_supports(Format format) noexcept { return vk_format(format) != 0; }

std::vector<std::byte> write_ktx2(const TextureImage& image, std::string_view writer) {
    if (!ktx2_supports(image.format)) throw std::invalid_argument("KTX2 cannot hold this texture format");
    const auto desc = image.desc();
    if (image.width == 0 || image.height == 0 || image.mip_levels == 0 ||
        image.mip_levels > full_mip_count(image.width, image.height) || image.data.size() != texture_bytes(desc))
        throw std::invalid_argument("KTX2 needs a complete image: every level, tightly packed");
    const auto levels = image.mip_levels;
    const auto dfd = data_format_descriptor(image.format);
    // Key/value data, sorted by key; each entry is padded to 4 bytes.
    auto kvd = std::vector<std::byte>{};
    const auto entry = [&](std::string_view key, std::string_view value) {
        const auto length = static_cast<uint32_t>(key.size() + 1 + value.size() + 1);
        const auto start = kvd.size();
        kvd.resize(align(start + 4 + length, 4));
        put32(kvd, start, length);
        std::memcpy(kvd.data() + start + 4, key.data(), key.size());
        std::memcpy(kvd.data() + start + 4 + key.size() + 1, value.data(), value.size());
    };
    entry("KTXorientation", "rd");
    entry("KTXwriter", writer);

    const auto dfd_offset = header_bytes + level_entry_bytes * levels;
    const auto kvd_offset = dfd_offset + dfd.size() * 4;
    auto offset = kvd_offset + kvd.size();
    // Data runs from the smallest level to level 0, each aligned.
    auto level_offsets = std::vector<size_t>(levels);
    for (auto level = levels; level-- > 0;) {
        offset = align(offset, level_alignment(image.format));
        level_offsets[level] = offset;
        offset += mip_level_bytes(image.format, image.width, image.height, level);
    }
    auto out = std::vector<std::byte>(offset);
    std::memcpy(out.data(), identifier.data(), identifier.size());
    const auto header = std::array<uint32_t, 9>{vk_format(image.format), 1, image.width, image.height, 0, 0, 1, levels, 0};
    for (size_t i = 0; i < header.size(); ++i) put32(out, 12 + 4 * i, header[i]);
    put32(out, 48, static_cast<uint32_t>(dfd_offset));
    put32(out, 52, static_cast<uint32_t>(dfd.size() * 4));
    put32(out, 56, static_cast<uint32_t>(kvd_offset));
    put32(out, 60, static_cast<uint32_t>(kvd.size()));
    put64(out, 64, 0); // no supercompression global data
    put64(out, 72, 0);
    auto source = size_t{0};
    for (uint32_t level = 0; level < levels; ++level) {
        const auto bytes = mip_level_bytes(image.format, image.width, image.height, level);
        const auto entry_offset = header_bytes + level_entry_bytes * level;
        put64(out, entry_offset, level_offsets[level]);
        put64(out, entry_offset + 8, bytes);
        put64(out, entry_offset + 16, bytes);
        std::memcpy(out.data() + level_offsets[level], image.data.data() + source, bytes);
        source += bytes;
    }
    for (size_t i = 0; i < dfd.size(); ++i) put32(out, dfd_offset + 4 * i, dfd[i]);
    std::memcpy(out.data() + kvd_offset, kvd.data(), kvd.size());
    return out;
}

Ktx2ReadResult read_ktx2(std::span<const std::byte> file) {
    auto result = Ktx2ReadResult{};
    const auto refuse = [&](std::string message) {
        result.error = std::move(message);
        result.image = {};
        return result;
    };
    if (file.size() < header_bytes) return refuse("the file is too short to be KTX2 (" + std::to_string(file.size()) + " bytes)");
    if (std::memcmp(file.data(), identifier.data(), identifier.size()) != 0) return refuse("the file does not start with the KTX2 identifier");
    const auto vk = get32(file, 12), type_size = get32(file, 16), width = get32(file, 20), height = get32(file, 24);
    const auto depth = get32(file, 28), layers = get32(file, 32), faces = get32(file, 36), levels = get32(file, 40);
    const auto supercompression = get32(file, 44);
    const auto code = std::ranges::find(format_codes, vk, &FormatCode::vk);
    if (code == format_codes.end())
        return refuse("vkFormat " + std::to_string(vk) + " is not one Maya reads (RGBA8 or ASTC 4x4/6x6 LDR, sRGB or linear)");
    const auto format = code->format;
    if (supercompression != 0) return refuse("supercompression scheme " + std::to_string(supercompression) + " is not supported");
    if (type_size != 1) return refuse("typeSize is " + std::to_string(type_size) + "; this format needs 1");
    if (width == 0 || height == 0 || depth != 0) return refuse("only 2D images are supported");
    if (width > ktx2_max_dimension || height > ktx2_max_dimension)
        return refuse("the image is " + std::to_string(width) + "x" + std::to_string(height) + "; Maya reads at most " +
                      std::to_string(ktx2_max_dimension) + " on each side");
    if (layers != 0 || faces != 1) return refuse("array layers and cube faces are not supported");
    if (levels == 0) return refuse("levelCount is 0 (mips to be generated at load), which Maya does not support");
    if (levels > full_mip_count(width, height))
        return refuse(std::to_string(levels) + " levels is more than a " + std::to_string(width) + "x" + std::to_string(height) + " image has");
    if (header_bytes + size_t{levels} * level_entry_bytes > file.size()) return refuse("the level index runs past the end of the file");

    // The data format descriptor must agree with vkFormat.
    const auto dfd_offset = size_t{get32(file, 48)}, dfd_length = size_t{get32(file, 52)};
    if (dfd_length < 28 || dfd_offset + dfd_length > file.size() || dfd_offset % 4 != 0)
        return refuse("the data format descriptor is missing or runs past the end of the file");
    if (get32(file, dfd_offset) != dfd_length) return refuse("the data format descriptor's size does not match the index");
    const auto model = get32(file, dfd_offset + 12) & 0xFF, transfer = (get32(file, dfd_offset + 12) >> 16) & 0xFF;
    if (model != (is_compressed_format(format) ? model_astc : model_rgbsda))
        return refuse("the data format descriptor's color model (" + std::to_string(model) + ") does not match vkFormat " + std::to_string(vk));
    if (transfer != (is_srgb_format(format) ? transfer_srgb : transfer_linear))
        return refuse(std::string("the data format descriptor's transfer function does not match vkFormat ") + std::to_string(vk) +
                      " (" + (is_srgb_format(format) ? "sRGB" : "linear") + ")");

    // Every level is checked against the file before anything is allocated for its texels.
    for (uint32_t level = 0; level < levels; ++level) {
        const auto entry = header_bytes + size_t{level} * level_entry_bytes;
        const auto offset = get64(file, entry), length = get64(file, entry + 8), uncompressed = get64(file, entry + 16);
        const auto expected = mip_level_bytes(format, width, height, level);
        if (length != expected || uncompressed != expected)
            return refuse("level " + std::to_string(level) + " holds " + std::to_string(length) + " bytes; " + format_name(format) + " needs " +
                          std::to_string(expected));
        if (offset > file.size() || length > file.size() - offset)
            return refuse("level " + std::to_string(level) + " runs past the end of the file");
        if (offset % level_alignment(format) != 0) return refuse("level " + std::to_string(level) + " is not aligned as KTX2 requires");
    }
    auto& image = result.image;
    image.format = format;
    image.width = width;
    image.height = height;
    image.mip_levels = levels;
    image.data.reserve(texture_bytes(image.desc()));
    for (uint32_t level = 0; level < levels; ++level) {
        const auto entry = header_bytes + size_t{level} * level_entry_bytes;
        const auto* begin = file.data() + get64(file, entry);
        image.data.insert(image.data.end(), begin, begin + get64(file, entry + 8));
    }
    return result;
}
} // namespace maya
