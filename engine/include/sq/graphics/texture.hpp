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
    // allocator: ステージングバッファ（HOST_VISIBLE, TRANSFER_SRC）の確保に使う（phase11 ③）。
    //   なおテクスチャ本体のイメージメモリは今フェーズではアロケータ非対応（従来通り
    //   vkAllocateMemory で確保する。イメージのアロケータ対応は将来課題）。
    Texture(VkPhysicalDevice physical_device, VkDevice device, GpuAllocator& allocator,
            std::uint32_t graphics_queue_family, VkQueue graphics_queue,
            const std::string& path);
    ~Texture();  // view -> image -> memory の順で破棄する

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // ディスクリプタ（combined image sampler）に渡すイメージビュー。
    [[nodiscard]] VkImageView view() const;

private:
    // old_layout -> new_layout のイメージメモリバリアを command_buffer に記録する。
    // 本クラスで使うのは以下の2遷移のみ:
    //   UNDEFINED            -> TRANSFER_DST_OPTIMAL     (転送前)
    //   TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL (転送後)
    static void transition_image_layout(VkCommandBuffer command_buffer, VkImage image,
                                        VkImageLayout old_layout, VkImageLayout new_layout);

    GpuAllocator* allocator_ = nullptr; // 破棄時に free するため保持 (所有しない)
    Allocation allocation_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
};

}  // namespace sq::graphics
