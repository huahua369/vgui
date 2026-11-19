#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace vgui::ui_design::detail {

// Streaming SHA-256 for file integrity checks, not authentication.
class sha256 {
public:
    void update(std::string_view bytes);
    // Returns the digest of all bytes seen so far without consuming the state.
    std::string hex_digest() const;

private:
    static constexpr std::size_t block_size = 64;
    void compress(const unsigned char* block);

    std::array<std::uint32_t, 8> state_{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::array<unsigned char, block_size> buffer_{};
    std::uint64_t total_bytes_ = 0;
};

} // namespace vgui::ui_design::detail
