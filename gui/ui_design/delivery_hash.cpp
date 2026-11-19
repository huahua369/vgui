#include "delivery_hash.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace vgui::ui_design::detail {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

} // namespace

void sha256::update(std::string_view bytes)
{
    if (bytes.empty()) return;

    const auto buffered = static_cast<std::size_t>(total_bytes_ % block_size);
    total_bytes_ += static_cast<std::uint64_t>(bytes.size());
    if (buffered != 0) {
        const auto count = std::min(block_size - buffered, bytes.size());
        std::memcpy(buffer_.data() + buffered, bytes.data(), count);
        bytes.remove_prefix(count);
        if (buffered + count != block_size) return;
        compress(buffer_.data());
    }

    while (bytes.size() >= block_size) {
        compress(reinterpret_cast<const unsigned char*>(bytes.data()));
        bytes.remove_prefix(block_size);
    }
    if (!bytes.empty()) std::memcpy(buffer_.data(), bytes.data(), bytes.size());
}

std::string sha256::hex_digest() const
{
    auto copy = *this;
    auto* block = copy.buffer_.data();
    auto end = static_cast<std::size_t>(total_bytes_ % block_size);
    block[end++] = 0x80u;

    // Reserve the final eight bytes for the original bit length, big-endian.
    constexpr auto length_offset = block_size - 8;
    if (end > length_offset) {
        std::fill(block + end, block + block_size, 0);
        copy.compress(block);
        end = 0;
    }
    std::fill(block + end, block + length_offset, 0);
    auto bit_length = total_bytes_ * 8u;
    for (auto i = block_size; i > length_offset;) {
        block[--i] = static_cast<unsigned char>(bit_length & 0xffu);
        bit_length >>= 8;
    }
    copy.compress(block);

    constexpr char hex[] = "0123456789abcdef";
    std::string digest(64, '0');
    std::size_t pos = 0;
    for (const auto word : copy.state_) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            digest[pos++] = hex[(word >> shift) & 0x0fu];
        }
    }
    return digest;
}

void sha256::compress(const unsigned char* block)
{
    std::array<std::uint32_t, 64> words;
    for (std::size_t i = 0; i < 16; ++i, block += 4) {
        words[i] = (std::uint32_t{block[0]} << 24) | (std::uint32_t{block[1]} << 16)
            | (std::uint32_t{block[2]} << 8) | std::uint32_t{block[3]};
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const auto x = words[i - 15];
        const auto y = words[i - 2];
        const auto s0 = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
        const auto s1 = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choose = (e & f) ^ (~e & g);
        const auto t1 = h + s1 + choose + round_constants[i] + words[i];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

} // namespace vgui::ui_design::detail
