#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "gpu_allocator.hpp"

namespace sq::graphics {

// シャドウマップ1枚ぶんの GPU リソース（phase16 ①-2）。
//
// DepthImage とよく似ているが、3つ違う:
//   1. usage に VK_IMAGE_USAGE_SAMPLED_BIT が要る（本パスのフラグメントシェーダが読むため）
//   2. 解像度がスワップチェーンと無関係（固定。リサイズで作り直さない）
//   3. VkFramebuffer も自分で持つ（描画先が1枚しか無いので、外に出す意味が無い）
//
// ★ DepthImage を継承したり流用したりしないこと。「深度イメージ」という共通点は
//   あるが、寿命の管理（片方はリサイズで作り直す・片方は作り直さない）が逆なので、
//   共通化すると recreate_swapchain が汚れる。
class ShadowMap {
public:
    // size: 一辺のテクセル数（正方形）。Renderer::kShadowMapSize（2048）を渡す。
    // depth_format: Renderer の depth_format_ をそのまま渡す。
    //   ★ D24_UNORM_S8_UINT などステンシル付きが選ばれている環境でも、
    //     ビューの aspectMask は VK_IMAGE_ASPECT_DEPTH_BIT **だけ**にすること
    //     （STENCIL を混ぜたビューはサンプルに使えない）。
    // shadow_render_pass: フレームバッファの互換性確認に使う（所有しない）。
    //   RenderPassConfig{ .has_color = false, ... } で作った深度専用パスを渡す。
    ShadowMap(VkDevice device, GpuAllocator& allocator,
              VkRenderPass shadow_render_pass, std::uint32_t size, VkFormat depth_format);
    ~ShadowMap();  // framebuffer -> view -> image -> memory の順で破棄

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    // set=2 binding=0 に書くビュー（imageLayout は DEPTH_STENCIL_READ_ONLY_OPTIMAL）。
    [[nodiscard]] VkImageView view() const;
    // シャドウパスの VkRenderPassBeginInfo::framebuffer に渡す。
    [[nodiscard]] VkFramebuffer framebuffer() const;
    // renderArea / vkCmdSetViewport / vkCmdSetScissor に使う。
    [[nodiscard]] VkExtent2D extent() const;

private:
    GpuAllocator* allocator_ = nullptr;  // 破棄時に free するため保持（所有しない。Device が所有）
    Allocation allocation_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
};

}  // namespace sq::graphics
