#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

#include "gpu_allocator.hpp"

namespace sq::graphics {

// HDR（Radiance .hdr）の equirectangular 画像を float の 2D テクスチャとして持つ（phase16 ②-2）。
//
// Texture に float 版のコンストラクタを足さずに別クラスにする理由:
//   - Texture::create_from_pixels は「1ピクセル 4 バイト」前提でステージングサイズを計算している。
//     流用すると 1/4 しか転送されず、下 3/4 が真っ黒になる（②-2 の罠）
//   - Texture はミップを生成するが、これは不要（equirect -> cube の変換で1回読むだけ）
//   - 寿命が違う。変換が終わったら**即座に破棄する**一時オブジェクト
//
// フォーマットは VK_FORMAT_R32G32B32A32_SFLOAT（stbi_loadf の出力をそのまま上げる）。
// 作り終えた時点で SHADER_READ_ONLY_OPTIMAL（読み手はコンピュートシェーダ）。
class HdrTexture {
public:
    // path: 実行時CWD基準（例: "textures/env/xxx_2k.hdr"）。
    //
    // ★ 読み込みに失敗したら**落とさずに**、1×1 の灰色（0.5, 0.5, 0.5, 1.0）で作ること
    //   （フェーズ全体の検証観点「HDR ファイル欠損でも落ちずにフォールバック」）。
    //   そのまま変換すれば「一様な灰色のキューブ」になり、IBL が地味なまま成立する。
    //   warn ログは必ず出す（黙って灰色になると「空が出ない」原因が分からない）。
    HdrTexture(VkDevice device, GpuAllocator& allocator,
               std::uint32_t graphics_queue_family, VkQueue graphics_queue,
               const std::string& path);
    ~HdrTexture();  // view -> image -> memory の順で破棄

    HdrTexture(const HdrTexture&) = delete;
    HdrTexture& operator=(const HdrTexture&) = delete;

    [[nodiscard]] VkImageView view() const;
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
