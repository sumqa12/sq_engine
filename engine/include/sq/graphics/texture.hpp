#pragma once

#include <string>

#include <vulkan/vulkan.h>

#include "gpu_allocator.hpp"

namespace sq::graphics {

class GpuAllocator;  // ステージングバッファの確保に使う（前方宣言）

// 画像ファイルをGPU上のサンプル可能なテクスチャ（VkImage）として保持するRAIIクラス。
// stb_imageで読み込んだピクセルを、ステージングバッファ経由でDEVICE_LOCALなVkImageへ転送し、
// シェーダから読める VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL まで遷移させる。
//
// VkImageはVkBuffer（Buffer基底）とは生成API系統が異なる（vkCreateImage /
// vkGetImageMemoryRequirements / vkBindImageMemory）ため、Bufferは継承しない。
// ただしメモリタイプ選択は Buffer::find_memory_type を再利用する（DepthImageと同様）。
class Texture {
public:
    // path の画像を読み込みGPUへ転送する。転送には使い捨てコマンドバッファを用い、
    // graphics_queue へ1回サブミットして完了を待つ（起動時に一度だけ実行される想定）。
    // allocator: ステージングバッファ（HOST_VISIBLE, TRANSFER_SRC）とイメージ本体の
    //   メモリ（DEVICE_LOCAL, non-linear）の確保に使う（phase11 ③ / phase13 ④）。
    // physical_device: メモリタイプ選択に加え、リニアフィルタ blit の対応判定にも使う（phase13 ③）。
    Texture(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
            std::uint32_t graphics_queue_family, VkQueue graphics_queue,
            const std::string& path);
    ~Texture();  // view -> image -> memory の順で破棄する

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // ディスクリプタ（combined image sampler）に渡すイメージビュー。
    [[nodiscard]] VkImageView view() const;

    // 生成されたミップレベル数（1 なら mip 無し）。phase13 ③
    [[nodiscard]] std::uint32_t mip_levels() const;

private:
    // old_layout -> new_layout のイメージメモリバリアを command_buffer に記録する。
    // phase13 ③: mip レベル範囲を指定できるようにした（生成中はレベルごとに遷移させるため）。
    //   base_mip_level から level_count 枚ぶんが対象になる。
    //
    // 必要な遷移パターン（old_layout で分岐する）:
    //   UNDEFINED            -> TRANSFER_DST_OPTIMAL     src: 0            / TOP_OF_PIPE
    //                                                    dst: TRANSFER_WRITE / TRANSFER
    //   TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL     src: TRANSFER_WRITE / TRANSFER   ★③で追加
    //                                                    dst: TRANSFER_READ  / TRANSFER
    //   TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL src: TRANSFER_READ  / TRANSFER   ★③で追加
    //                                                    dst: SHADER_READ    / FRAGMENT_SHADER
    //   TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL src: TRANSFER_WRITE / TRANSFER
    //                                                    dst: SHADER_READ    / FRAGMENT_SHADER
    // ★ old_layout だけでは TRANSFER_DST からの2遷移を区別できないので、new_layout も見て分岐すること。
    static void transition_image_layout(VkCommandBuffer command_buffer, VkImage image,
                                        VkImageLayout old_layout, VkImageLayout new_layout,
                                        std::uint32_t base_mip_level, std::uint32_t level_count);

    // ミップマップを vkCmdBlitImage の連鎖で生成する（phase13 ③ / D-2）。
    // 呼ぶ前提: 全レベルが TRANSFER_DST_OPTIMAL で、レベル0だけピクセルが埋まっていること。
    // 抜けた後: 全レベルが SHADER_READ_ONLY_OPTIMAL になっていること。
    //
    // 手順（レベル i = 1 .. mip_levels_ - 1 について繰り返す。w, h は「レベル i-1 のサイズ」）:
    //   1. レベル i-1 を TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL へ遷移
    //      （直前の blit の書き込み完了を待つ意味も兼ねる。ここを飛ばすと競合する）
    //   2. VkImageBlit を作る:
    //        srcSubresource = { COLOR, mipLevel = i-1, baseArrayLayer = 0, layerCount = 1 }
    //        srcOffsets[0] = {0, 0, 0}, srcOffsets[1] = {w, h, 1}
    //        dstSubresource = { COLOR, mipLevel = i,   baseArrayLayer = 0, layerCount = 1 }
    //        dstOffsets[0] = {0, 0, 0}, dstOffsets[1] = {max(w/2, 1), max(h/2, 1), 1}
    //      vkCmdBlitImage(command_buffer, image_, TRANSFER_SRC_OPTIMAL,
    //                                     image_, TRANSFER_DST_OPTIMAL,
    //                     1, &blit, VK_FILTER_LINEAR);   // ★ src と dst が同じイメージ
    //   3. レベル i-1 を TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL へ遷移（もう使わないので確定）
    //   4. w = max(w / 2, 1); h = max(h / 2, 1);
    // ループ後: 最後のレベル（mip_levels_ - 1）は blit の書き込み先のままなので、
    //   TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL へ個別に遷移させる（★忘れやすい）。
    void generate_mipmaps(VkCommandBuffer command_buffer,
                          std::int32_t width, std::int32_t height);

    // フォーマットがリニアフィルタ blit に対応しているか（phase13 D-2）。
    // vkGetPhysicalDeviceFormatProperties の optimalTilingFeatures に
    // VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT が立っているかで判定する。
    // 非対応なら mip_levels_ = 1 にフォールバックする（落とさない）。
    [[nodiscard]] static bool supports_linear_blit(VkPhysicalDevice physical_device, VkFormat format);

    GpuAllocator* allocator_ = nullptr; // 破棄時に free するため保持 (所有しない)
    Allocation allocation_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    std::uint32_t mip_levels_ = 1;  // phase13 ③。1 は「mip 無し」を意味する
};

}  // namespace sq::graphics
