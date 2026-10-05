#include "maya/core/sha256.hpp"
#include <algorithm>
#include <cstring>

namespace maya {
namespace {
constexpr std::array<uint32_t, 64> round_constants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
constexpr uint32_t rotate(uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }
} // namespace

Sha256::Sha256() noexcept
    : m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const uint8_t* data) noexcept {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(data[4 * i]) << 24 | uint32_t(data[4 * i + 1]) << 16 | uint32_t(data[4 * i + 2]) << 8 | uint32_t(data[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const auto s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = m_state;
    for (int i = 0; i < 64; ++i) {
        const auto t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + round_constants[size_t(i)] + w[i];
        const auto t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    const uint32_t result[8] = {a, b, c, d, e, f, g, h};
    for (size_t i = 0; i < 8; ++i) m_state[i] += result[i];
}

void Sha256::update(std::span<const std::byte> data) noexcept {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    auto size = data.size();
    m_length += size;
    if (m_buffered > 0) {
        const auto take = std::min(size, 64 - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, bytes, take);
        m_buffered += take;
        bytes += take;
        size -= take;
        if (m_buffered < 64) return;
        block(m_buffer.data());
        m_buffered = 0;
    }
    for (; size >= 64; bytes += 64, size -= 64) block(bytes);
    std::memcpy(m_buffer.data(), bytes, size);
    m_buffered = size;
}

Sha256Digest Sha256::finish() noexcept {
    const auto bits = m_length * 8;
    m_buffer[m_buffered++] = 0x80;
    if (m_buffered > 56) {
        std::memset(m_buffer.data() + m_buffered, 0, 64 - m_buffered);
        block(m_buffer.data());
        m_buffered = 0;
    }
    std::memset(m_buffer.data() + m_buffered, 0, 56 - m_buffered);
    for (int i = 0; i < 8; ++i) m_buffer[size_t(56 + i)] = uint8_t(bits >> (56 - 8 * i));
    block(m_buffer.data());
    auto digest = Sha256Digest{};
    for (size_t i = 0; i < 8; ++i)
        for (size_t j = 0; j < 4; ++j) digest[4 * i + j] = uint8_t(m_state[i] >> (24 - 8 * j));
    return digest;
}

Sha256Digest sha256(std::span<const std::byte> data) noexcept {
    auto hasher = Sha256{};
    hasher.update(data);
    return hasher.finish();
}

std::string sha256_text(const Sha256Digest& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    auto text = std::string{};
    for (const auto byte : digest) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}
} // namespace maya
