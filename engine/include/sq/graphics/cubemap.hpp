#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_allocator.hpp"

namespace sq::graphics {

// キューブマップ（6面のレイヤを持つ VkImage）の RAII ラッパー（phase16 ②-1 / D-8）。
//
// Texture とは分ける。
//   Texture は「ファイル／ピクセル列から作って SHADER_READ_ONLY で固定」だが、
//   Cubemap は「コンピュートが GENERAL レイアウトで書き込み、書き終えたら
//   SHADER_READ_ONLY へ遷移する」という別の寿命を持つ。
//
// ★ コンストラクタはイメージとビューを作るだけで、**中身もレイアウトも未定義のまま**。
//   書き込み（UNDEFINED -> GENERAL -> dispatch -> SHADER_READ_ONLY）は使う側
//   （EnvironmentMap）が SingleTimeCommands の中で行う。
class Cubemap {
public:
    // size:   1面の一辺（正方形）。★ コンピュートの local_size（8）の倍数にすること（②-4）
    // mips:   mip レベル数（prefiltered specular で 5。それ以外は 1）
    // format: VK_FORMAT_R16G16B16A16_SFLOAT（D-8。HDR かつストレージイメージに使えるため）
    //
    // ★ VkImageCreateInfo に必要なもの（どれか1つでも抜けると動かない）:
    //     arrayLayers = 6
    //     flags       = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT   ← 忘れると CUBE ビューの作成でエラー
    //     usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    Cubemap(VkDevice device, GpuAllocator& allocator,
            std::uint32_t size, std::uint32_t mips, VkFormat format);
    ~Cubemap();  // storage_views -> sample_view -> image -> memory の順で破棄

    Cubemap(const Cubemap&) = delete;
    Cubemap& operator=(const Cubemap&) = delete;

    // サンプル用ビュー（VK_IMAGE_VIEW_TYPE_CUBE, 全 mip, layerCount = 6）。
    // set=2 の binding=1 / binding=2 と、irradiance / prefilter の入力に使う。
    [[nodiscard]] VkImageView sample_view() const;

    // 書き込み用ビュー（VK_IMAGE_VIEW_TYPE_2D_ARRAY, 単一 mip, layerCount = 6）。
    //
    // ★ mip ごとに別のビューが要る。imageStore は mip レベルを指定できないため、
    //   「どの mip に書くか」はビューが決める（D-8）。コンストラクタで mips 本ぶん
    //   まとめて作っておくこと。
    // ★ GLSL 側は **image2DArray** で受けること（z = 面番号 = 配列の層）。
    //   imageCube（Dim = Cube）で受けると、ビューの型の不一致でバリデーションエラーになる
    //   （Cube は VIEW_TYPE_CUBE にしか合わない）。
    [[nodiscard]] VkImageView storage_view(std::uint32_t mip) const;

    [[nodiscard]] VkImage handle() const;   // レイアウト遷移に使う
    [[nodiscard]] std::uint32_t size() const;
    [[nodiscard]] std::uint32_t mips() const;

private:
    GpuAllocator* allocator_ = nullptr;  // 所有しない（Device が所有）
    Allocation allocation_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView sample_view_ = VK_NULL_HANDLE;
    std::vector<VkImageView> storage_views_;  // mip ごと
    std::uint32_t size_ = 0;
    std::uint32_t mips_ = 1;
};

}  // namespace sq::graphics
