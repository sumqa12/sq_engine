#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_allocator.hpp"

namespace sq::graphics {

// ShadowMap の作り分け（phase17 ⓪-5 / D-2）。
//
//   用途             layers                   sample_view_type
//   phase16 の方向光 1                        VK_IMAGE_VIEW_TYPE_2D          （⓪-5 はこれで phase16 を再現する）
//   ② CSM           kCascadeCount（4）        VK_IMAGE_VIEW_TYPE_2D_ARRAY
//   ③ 点光源        6 * kMaxPointShadows（24） VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
//
// 3つとも「サンプルするときは全層を1本のビューで読み、描くときは1層ずつ別のフレームバッファへ描く」
// という同じ形をしている。違うのは層数とサンプル用ビューの型だけ。
struct ShadowMapConfig {
    std::uint32_t size = 2048;                                // 1層の一辺（正方形）
    std::uint32_t layers = 1;                                 // 配列の層数
    VkImageViewType sample_view_type = VK_IMAGE_VIEW_TYPE_2D; // サンプル用ビューの型

    // ★ CUBE / CUBE_ARRAY のときは VkImageCreateInfo::flags に VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT が要る
    //   （phase16 D-8 のキューブマップと同じ）。フラグを別メンバにせず、sample_view_type から決めること
    //   （2か所に書くと食い違う）。
    // ★ CUBE_ARRAY なら layers は 6 の倍数でなければならない（③-2）。
};

// 層を持つシャドウマップ（phase16 ①-2、phase17 ⓪-5 で層対応）。
//
// DepthImage とよく似ているが、3つ違う:
//   1. usage に VK_IMAGE_USAGE_SAMPLED_BIT が要る（本パスのフラグメントシェーダが読むため）
//   2. 解像度がスワップチェーンと無関係（固定。リサイズで作り直さない）
//   3. VkFramebuffer も自分で持つ（層ごとに1枚。外に出す意味が無い）
//
// ★ DepthImage を継承したり流用したりしないこと。「深度イメージ」という共通点は
//   あるが、寿命の管理（片方はリサイズで作り直す・片方は作り直さない）が逆なので、
//   共通化すると recreate_swapchain が汚れる。
class ShadowMap {
public:
    // queue_family_index / queue: 初期遷移（下記）の SingleTimeCommands に使う（phase17 ⓪-5）。
    // shadow_render_pass: フレームバッファの互換性確認に使う（所有しない）。
    //   RenderPassConfig{ .has_color = false, ... } で作った深度専用パスを渡す。
    // depth_format: Renderer の depth_format_ をそのまま渡す。
    //   ★ D24_UNORM_S8_UINT などステンシル付きが選ばれている環境でも、
    //     ビューの aspectMask は VK_IMAGE_ASPECT_DEPTH_BIT **だけ**にすること
    //     （STENCIL を混ぜたビューはサンプルに使えない）。
    //
    // ★ コンストラクタの最後で**全層を DEPTH_STENCIL_READ_ONLY_OPTIMAL へ遷移しておく**（phase17 ⓪-5）。
    //   サンプル用ビューは全層を指す。一度も描かれていない層が UNDEFINED のまま残ると、
    //   本パスがそのビューを読んだ瞬間に「レイアウト不一致」のバリデーションが出る。
    //   ③ の点光源は使われていないスロットの層を毎フレーム描かないので、これが無いと必ず踏む。
    //   初期遷移があれば、**描くものが無い層はパスごと飛ばしてよい**。
    ShadowMap(VkDevice device, GpuAllocator& allocator,
              std::uint32_t queue_family_index, VkQueue queue,
              VkRenderPass shadow_render_pass, VkFormat depth_format,
              const ShadowMapConfig& config);
    ~ShadowMap();  // framebuffers -> layer views -> sample view -> image -> memory の順で破棄

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    // set=2 に書くサンプル用ビュー（全層。imageLayout は DEPTH_STENCIL_READ_ONLY_OPTIMAL）。
    [[nodiscard]] VkImageView sample_view() const;
    // layer 番目の層に描くフレームバッファ。シャドウパスの VkRenderPassBeginInfo::framebuffer に渡す。
    //   ★ 範囲外は例外（.at()）。③ で「slot * 6 + face」を計算し間違えたときに、黙って別の層へ描かないように。
    [[nodiscard]] VkFramebuffer framebuffer(std::uint32_t layer) const;
    // renderArea / vkCmdSetViewport / vkCmdSetScissor に使う（全層で同じ）。
    [[nodiscard]] VkExtent2D extent() const;
    [[nodiscard]] std::uint32_t layers() const;

private:
    GpuAllocator* allocator_ = nullptr;  // 破棄時に free するため保持（所有しない。Device が所有）
    Allocation allocation_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;

    // サンプル用（全層。viewType = config.sample_view_type, baseArrayLayer = 0, layerCount = layers）。
    VkImageView sample_view_ = VK_NULL_HANDLE;
    // 描画用（層ごと。viewType = 2D, baseArrayLayer = i, layerCount = 1）。framebuffers_ と同じ数。
    std::vector<VkImageView> layer_views_;
    // 層ごと。framebuffers_[i] のアタッチメントは layer_views_[i]。
    std::vector<VkFramebuffer> framebuffers_;

    VkExtent2D extent_{};
    std::uint32_t layers_ = 1;
};

}  // namespace sq::graphics
