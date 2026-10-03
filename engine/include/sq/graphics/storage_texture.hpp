#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "gpu_allocator.hpp"

namespace sq::graphics {

// コンピュートで書き込み、シェーダで読む 2D テクスチャ（phase16 ②-7。BRDF LUT 用）。
//
// Texture に usage を渡せるようにする案もあるが、採らない:
//   Texture は「ファイル／ピクセル列から作る・ミップを生成する・TRANSFER で上げる」が本体で、
//   こちらは「中身はコンピュートが書く・ミップ無し・TRANSFER を使わない」。共通部分がほぼ無い。
// Cubemap の 2D・1層・1 mip 版と考えればよい（ビューは1本で、書き込みにも読み取りにも使う）。
//
// ★ コンストラクタはイメージとビューを作るだけで、**中身もレイアウトも未定義のまま**
//   （Cubemap と同じ）。UNDEFINED -> GENERAL -> dispatch -> SHADER_READ_ONLY は使う側が行う。
class StorageTexture {
public:
    // format: ★ ストレージイメージとして書くので、GLSL の書式修飾子と一致させること。
    //   BRDF LUT は R16G16_SFLOAT（rg16f）。これは「拡張ストレージ書式」で、
    //   デバイス機能 shaderStorageImageExtendedFormats の有効化と、フォーマットの
    //   STORAGE_IMAGE_BIT 対応の両方が要る（Device::is_supported_storage_image_extended_formats）。
    //   ★ rgba16f / rgba32f / r32f などの「基本書式」はこの機能なしで使える。
    StorageTexture(VkDevice device, GpuAllocator& allocator,
                   std::uint32_t width, std::uint32_t height, VkFormat format);
    ~StorageTexture();  // view -> image -> memory の順で破棄

    StorageTexture(const StorageTexture&) = delete;
    StorageTexture& operator=(const StorageTexture&) = delete;

    // 書き込み（STORAGE_IMAGE / GENERAL）にも読み取り（COMBINED_IMAGE_SAMPLER / SHADER_READ_ONLY）にも使う。
    // ★ 1 mip・1層なので、Cubemap のように用途別のビューを分ける必要が無い。
    [[nodiscard]] VkImageView view() const;
    [[nodiscard]] VkImage handle() const;
    [[nodiscard]] std::uint32_t width() const;
    [[nodiscard]] std::uint32_t height() const;

private:
    GpuAllocator* allocator_ = nullptr;  // 所有しない
    Allocation allocation_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

}  // namespace sq::graphics
