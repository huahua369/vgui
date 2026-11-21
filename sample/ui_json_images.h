#pragma once

// Sample-only PNG/JPEG loading shared by the standalone JSON hosts.
#include <vgui/vgui.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS 4096
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

namespace vgui_sample {

inline std::vector<std::uint8_t> read_asset(const std::filesystem::path& file, std::size_t limit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size == 0 || size > limit) return {};
    std::ifstream stream(file, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) ||
        stream.peek() != std::char_traits<char>::eof() || stream.bad()) return {};
    return bytes;
}

struct json_image_decoder {
    vgui_texture* load(vgui_context* ctx, const std::filesystem::path& file) {
        // The runtime already resolved the resource to a contained regular file.
        const auto bytes = read_asset(file, max_image_bytes);
        return load(ctx, std::span<const std::uint8_t>(bytes));
    }

    vgui_texture* load(vgui_context* ctx, std::span<const std::uint8_t> bytes) {
        if (bytes.empty() || bytes.size() > max_image_bytes) return nullptr;
        int w = 0, h = 0, channels = 0;
        const int count = static_cast<int>(bytes.size());
        if (!stbi_info_from_memory(bytes.data(), count, &w, &h, &channels) || w < 1 || h < 1 ||
            w > STBI_MAX_DIMENSIONS || h > STBI_MAX_DIMENSIONS) return nullptr;
        const auto rgba_bytes = std::size_t(w) * h * 4;
        if (rgba_bytes > max_mount_bytes - decoded) return nullptr;
        decoded += rgba_bytes;
        int decoded_w = 0, decoded_h = 0;
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load_from_memory(
            bytes.data(), count, &decoded_w, &decoded_h, &channels, STBI_rgb_alpha), stbi_image_free);
        if (!pixels || decoded_w != w || decoded_h != h) return nullptr;
        const vgui_texture_desc desc{w, h, w * 4, VGUI_TEXTURE_FORMAT_RGBA8, pixels.get()};
        image_ptr texture(vgui_texture_create(ctx, &desc), vgui_texture_destroy);
        auto* result = texture.get();
        if (result) textures.push_back(std::move(texture));
        return result;
    }

private:
    static constexpr std::size_t max_image_bytes = 16u << 20;
    static constexpr std::size_t max_mount_bytes = 64u << 20;
    using image_ptr = std::unique_ptr<vgui_texture, decltype(&vgui_texture_destroy)>;
    std::vector<image_ptr> textures;
    std::size_t decoded = 0;
};

} // namespace vgui_sample
