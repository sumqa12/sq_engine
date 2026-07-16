#pragma once

#include <vector>

#include <vulkan/vulkan.h>

namespace sq::graphics {

// 表示中／レンダリング中のスワップチェーン画像への上書きを防ぐために、
// セマフォ（GPU間同期）およびフェンス（CPU-GPU同期）を保持する。
// 個数とインデックスの対応に注意:
//  - image_available / in_flight_fence: frames_in_flight 個。current_frame_ でインデックス
//    （CPU側のフレームペーシング用）。
//  - render_finished: swapchain_image_count 個。image_index でインデックス
//    （プレゼンテーションが待つ「画像単位」のセマフォのため。フレーム数(2)と
//     画像枚数(3)がずれると、まだ使用中のセマフォを再シグナルしてしまう）。
class SyncObjects {
public:
    SyncObjects(VkDevice device, std::size_t frames_in_flight, std::size_t swapchain_image_count);
    ~SyncObjects();

    SyncObjects(const SyncObjects&) = delete;
    SyncObjects& operator=(const SyncObjects&) = delete;

    [[nodiscard]] VkSemaphore image_available(std::size_t frame_index) const;
    // 注意: 引数はcurrent_frame_ではなく、vkAcquireNextImageKHRが返したimage_indexを渡すこと。
    [[nodiscard]] VkSemaphore render_finished(std::size_t image_index) const;
    [[nodiscard]] VkFence in_flight_fence(std::size_t frame_index) const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::vector<VkFence> in_flight_fences_;
};

}  // namespace sq::graphics
