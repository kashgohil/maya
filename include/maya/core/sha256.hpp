#pragma once
// SHA-256 (FIPS 180-4), for content keys such as the cook cache's (docs/assets.md#cook-cache).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace maya {
using Sha256Digest = std::array<uint8_t, 32>;

/// Hashes data given in any number of pieces.
class Sha256 {
public:
    Sha256() noexcept;
    void update(std::span<const std::byte> data) noexcept;
    void update(std::string_view text) noexcept { update(std::as_bytes(std::span(text.data(), text.size()))); }
    /// The digest of everything given; the hasher is then spent.
    Sha256Digest finish() noexcept;

private:
    void block(const uint8_t* data) noexcept;
    std::array<uint32_t, 8> m_state;
    std::array<uint8_t, 64> m_buffer{};
    size_t m_buffered = 0;
    uint64_t m_length = 0; // bytes
};
Sha256Digest sha256(std::span<const std::byte> data) noexcept;
/// 64 lowercase hexadecimal digits.
std::string sha256_text(const Sha256Digest& digest);
} // namespace maya
